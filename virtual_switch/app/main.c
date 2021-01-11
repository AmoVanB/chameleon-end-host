/**
 * DPDK app implementing the Chameleon virtual switch.
 * The switch performs tagging and shaping before sending
 * packets to the NIC.
 *
 * Amaury Van Bemten <amaury.van-bemten@tum.de>
 * Nemanja Deric <nemanja.deric@tum.de>
 */
#include <arpa/inet.h>
#include <getopt.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>
#include <signal.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/param.h>
#include <unistd.h>
#include <stdbool.h>

#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_string_fns.h>
#include <rte_malloc.h>
#include <rte_vhost.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_pause.h>

/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_VHOST_CONFIG RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_VHOST_DATA   RTE_LOGTYPE_USER2
#define RTE_LOGTYPE_VHOST_PORT   RTE_LOGTYPE_USER3

/* Types of queues */
enum {
	/* RX queue */
	VIRTIO_RXQ,
	/* TX queue */
	VIRTIO_TXQ,
	/* Number (2) of queue types for IDs: rx_queue_id * VIRTIO_QNUM + VIRTIO_TXQ */
	VIRTIO_QNUM
};

/* five-tuple matching entries per vHost */
#define N_ENTRIES_PER_VHOST 3

/* Max number of VLAN tags to push */
#define N_TAGS 10

struct vlan_hdr {
    uint16_t eth_type;
    uint16_t vlan_id;
};

/* Structure of a matching table entry */
struct tagging_entry {
	uint8_t protocol;
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t src_port;
	uint16_t dst_port;
	uint64_t rate_bps; /* rate in bps */
	uint64_t burst_bits; /* burst in bits  */
	uint64_t n_tokens; /* tokens are actually burst * cpu_frequency */
	uint64_t last_tsc; /* timer - type of rte_rdtsc() */
	uint16_t n_tags;
	struct vlan_hdr tags[N_TAGS];
};

#define MAX_VIRTIO_DEVICES 64
#define DEBUG_SHAPER 1
/*
 * First dimension: pool id (corresponds to a device)
 * Second dimension: a list of five-tuple matchings for each vHost
 */
static struct tagging_entry matching_table[MAX_VIRTIO_DEVICES + 1][N_ENTRIES_PER_VHOST]; // +1 for the 0 entry unused by the control VM
static uint64_t cpu_freq = 0;

/* Max burst size for RX/TX */
#define MAX_PKT_BURST 32

/* Device statistics */
struct device_statistics {
	/* Number of packets received from vHost */
	uint64_t	tx_total;

	/* Number of packets received from vHost and properly tagged */
	uint64_t	tx_tagged;

	/* Number of packets dropped by shaper */
	uint64_t 	tx_dropped;

	/* Number of packets received from vHost and forwarded */
	uint64_t	tx_success;
	
	/* Number of packets received in the RX queue of vHost */
	rte_atomic64_t	rx_total_atomic;
	/* Number of packets transmitted to vHost */
	rte_atomic64_t	rx_success_atomic;
};

/* vHost device representation */
struct vhost_dev {
	/* Device MAC address (Obtained on first TX packet) */
	struct rte_ether_addr mac_address;
	/* The VMDQ pool_id of the dev */
	uint16_t pool_id;
	/* RX VMDQ queue number (could be derived from pool_id) */
	uint16_t vmdq_rx_q;
	/* Vlan tag assigned to the pool */
	uint32_t vlan_tag;
	/* Core sending data for this vdev */
	uint16_t tx_coreid;
	/* Core receiving data for this vdev */
	uint16_t rx_coreid;
	/* A device is set as ready if the MAC address has been set */
	volatile uint8_t ready;
	/* Device is marked for removal from the data core */
	volatile uint8_t remove;
	/* Device id */
	int vid;
	/* Device stats */
	struct device_statistics stats;
	/* Defines "next" tail queue elements */
	TAILQ_ENTRY(vhost_dev) global_vdev_entry; // in the global queue
	TAILQ_ENTRY(vhost_dev) tx_lcore_vdev_entry; // in the per-TX_lcore queue
	TAILQ_ENTRY(vhost_dev) rx_lcore_vdev_entry; // in the per-TX_lcore queue
} __rte_cache_aligned;

/* Defines "struct vhost_dev_tailq_list" as a tail queue of "struct vhost_dev" */
TAILQ_HEAD(vhost_dev_tailq_list, vhost_dev);

/* Data core specific information. */
struct lcore_info {
	/* Number of devices handled by the core */
	uint32_t		device_num;
	/* Flag to synchronize device removal */
#define REQUEST_DEV_REMOVAL	1
#define ACK_DEV_REMOVAL		0
	volatile uint8_t	dev_removal_flag;
	/* List of vHost handled by the core (TX) */
	struct vhost_dev_tailq_list tx_vdev_list;
	/* List of vHost handled by the core (RX) */
	struct vhost_dev_tailq_list rx_vdev_list;
};


#define VLAN_HLEN       4

/* State of virtio device (learning MAC of VM, working data, working control, ready to delete) */
#define DEVICE_MAC_LEARNING 0
#define DEVICE_DATA_RX		1
#define DEVICE_CONTROL		2
#define DEVICE_SAFE_REMOVE	3

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_TX_DESC_DEFAULT 512 
#define RTE_TEST_RX_DESC_DEFAULT 2048

/* Maximum long option length for option parsing. */
#define MAX_LONG_OPT_SZ 64

/* EtherType reversed so that CPU stores in BE */
#define BE_RTE_ETHER_TYPE_IPV4 0x0008
#define BE_RTE_ETHER_TYPE_VLAN 0x0081

/* Promiscuous mode */
static uint32_t promiscuous;

/* Number of devices/queues to support */
static uint32_t num_queues = 0;
static uint32_t num_virtio_devices;

/* Mempool for the mbufs (message buffers) used by the applcation */
static struct rte_mempool *mbuf_pool;

/* Enable TX checksum offload */
static uint32_t enable_tx_csum = 1;
/* Client or server mode */
static int client_mode = 0;
/* Enable dequeue zero copy */
static int dequeue_zero_copy;
/* Enable tagging */
static uint32_t do_tag = 1;
/* Enable shaping */
static uint32_t do_shape = 1;
static int pool_allocation_failure = 0;

/* Socket file paths */
static char *socket_files;
static int nb_sockets;

/* VMDq configuration structure */
static struct rte_eth_conf vmdq_conf_default = {
	.rxmode = {
		/* How to route packets? VMDq (MAC & VLAN-based)
		 * We do not use RSS (hash-based) or DCB (VLAN priority based) */
		.mq_mode        = ETH_MQ_RX_VMDQ_ONLY,
		.split_hdr_size = 0,
		/* VLAN strip is necessary for 1G NIC such as I350,
		 * this fixes bug of ipv4 forwarding in guest can't
		 * forward pakets from one virtio dev to another virtio dev */
		.offloads = DEV_RX_OFFLOAD_VLAN_STRIP,
	},

	/* Normal TX with full offload */
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
		.offloads = (
				DEV_TX_OFFLOAD_IPV4_CKSUM |
				DEV_TX_OFFLOAD_TCP_CKSUM |
				DEV_TX_OFFLOAD_VLAN_INSERT |
				DEV_TX_OFFLOAD_MULTI_SEGS |
				DEV_TX_OFFLOAD_TCP_TSO),
	},
	
	.rx_adv_conf = {
		/* Should be overridden separately in code with
		 * appropriate values */
		.vmdq_rx_conf = {
			.nb_queue_pools = ETH_8_POOLS,
			.enable_default_pool = 0,
			.default_pool = 0,
			.nb_pool_maps = 0,
			.pool_map = {{0, 0},},
		},
	},
};

/* Logical core IDs and data */
static unsigned lcore_ids[RTE_MAX_LCORE];
static struct lcore_info lcore_info[RTE_MAX_LCORE];

/* DPDK port used */ 
static int32_t used_port_id;

/* Port information */
static uint16_t num_pf_queues, num_vmdq_queues;
static uint16_t vmdq_pool_base, vmdq_queue_base;
static uint16_t queues_per_pool;

/* For each pool ID, VLAN tag to use */
const uint16_t vlan_tags[] = {
	 1,  2,  3,  4,  5,  6,  7,  8,
	 9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24,
	25, 26, 27, 28, 29, 30, 31, 32,
	33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48,
	49, 50, 51, 52, 53, 54, 55, 56,
	57, 58, 59, 60, 61, 62, 63, 64
};

/* Which pools are used */
static bool pools_used[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0
};

/* MAC 00 is control channel, should not get a pool */
#define GET_POOL_ID(mac_address) (mac_address.addr_bytes[5] % (num_virtio_devices + 1)) - 1

/* List of VirtIO devices */
static struct vhost_dev_tailq_list vhost_dev_list = TAILQ_HEAD_INITIALIZER(vhost_dev_list);

/* Used for queueing bursts of TX packets. */
struct mbuf_table {
	unsigned len;
	unsigned txq_id;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

/* TX queue for each data core. */
struct mbuf_table lcore_tx_queue[RTE_MAX_LCORE];

/* Print out the matching table */
static void
print_table(void)
{
	struct vhost_dev *vdev;
	uint16_t entry_id;
	
	// TODO: not hardcode N_TAGS
	RTE_LOG(INFO, VHOST_DATA, "**Matching table**\n");
	RTE_LOG(INFO, VHOST_DATA, "=====  =======  =====  =================  =================  =======  =======  ========  ============  =============  ===========================================================\n");
	RTE_LOG(INFO, VHOST_DATA, " vID    rule     pro       ip_source       ip_destination     sport    dport    n_tags    burst_bits     rate_bps                                tags_list\n");
	RTE_LOG(INFO, VHOST_DATA, "-----  -------  -----  -----------------  -----------------  -------  -------  --------  ------------  -------------  --------------------------------------------------------------\n");
	
	TAILQ_FOREACH(vdev, &vhost_dev_list, global_vdev_entry) {
		if(vdev->ready == DEVICE_DATA_RX) {
			for(entry_id = 0; entry_id < N_ENTRIES_PER_VHOST; entry_id++) {
				RTE_LOG(INFO, VHOST_DATA, " %3u    %5u    %3u    %3u.%3u.%3u.%3u    %3u.%3u.%3u.%3u    %5u    %5u   %7u    %11lu    %11lu    %5u,%5u,%5u,%5u,%5u,%5u,%5u,%5u,%5u,%5u\n",
				vdev->vid,
				entry_id,
				matching_table[vdev->vlan_tag][entry_id].protocol,
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip >> 8)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip >> 16)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip >> 24)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip >> 8)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip >> 16)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip >> 24)),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].src_port),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].dst_port),
     				matching_table[vdev->vlan_tag][entry_id].n_tags,
				matching_table[vdev->vlan_tag][entry_id].burst_bits,
				matching_table[vdev->vlan_tag][entry_id].rate_bps,
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[0].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[1].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[2].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[3].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[4].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[5].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[6].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[7].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[8].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[9].vlan_id));
			}
		}
	}
	RTE_LOG(INFO, VHOST_DATA, "=====  =======  =====  =================  =================  =======  =======  ========  ============  =============  ==============================================================\n");
	
	// parsable version
	TAILQ_FOREACH(vdev, &vhost_dev_list, global_vdev_entry) {
		if(vdev->ready == DEVICE_DATA_RX) {
			for(entry_id = 0; entry_id < N_ENTRIES_PER_VHOST; entry_id++) {
				RTE_LOG(INFO, VHOST_DATA, "parsable-matching_table=%u-%u-%u-%u.%u.%u.%u-%u.%u.%u.%u-%u-%u-%u-%lu-%lu-%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
				vdev->vid,
				entry_id,
				matching_table[vdev->vlan_tag][entry_id].protocol,
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip >> 8)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip >> 16)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].src_ip >> 24)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip >> 8)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip >> 16)),
				((uint8_t) (matching_table[vdev->vlan_tag][entry_id].dst_ip >> 24)),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].src_port),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].dst_port),
     				matching_table[vdev->vlan_tag][entry_id].n_tags,
     				matching_table[vdev->vlan_tag][entry_id].burst_bits,
     				matching_table[vdev->vlan_tag][entry_id].rate_bps,
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[0].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[1].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[2].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[3].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[4].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[5].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[6].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[7].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[8].vlan_id),
				rte_be_to_cpu_16(matching_table[vdev->vlan_tag][entry_id].tags[9].vlan_id));
			}
		}
	}
}

static void
print_stats(void)
{
		struct vhost_dev *vdev;

		RTE_LOG(INFO, VHOST_DATA, "**Tagging application statistics**\n");
		RTE_LOG(INFO, VHOST_DATA, "=====  ======  ===================  =====  =======  ============  ============  ============  ============  ============  ============\n");
		RTE_LOG(INFO, VHOST_DATA, " vID    vlan       mac_address       RXq    TX/RX    rx_packets    rx_success    tx_packets    tx_success    tx_tagged     tx_dropped  \n");
		RTE_LOG(INFO, VHOST_DATA, "-----  ------  -------------------  -----  -------  ------------  ------------  ------------  ------------  ------------  ------------\n");
		TAILQ_FOREACH(vdev, &vhost_dev_list, global_vdev_entry) {
		
			RTE_LOG(INFO, VHOST_DATA, " %3u   %5u    %02x:%02x:%02x:%02x:%02x:%02x    %3u    %2u/%2u %13"PRIu64" %13"PRIu64" %13"PRIu64" %13"PRIu64" %13"PRIu64" %13"PRIu64"\n",
							vdev->vid,
							vdev->vlan_tag,
							vdev->mac_address.addr_bytes[0],
							vdev->mac_address.addr_bytes[1],
							vdev->mac_address.addr_bytes[2],
							vdev->mac_address.addr_bytes[3],
							vdev->mac_address.addr_bytes[4],
							vdev->mac_address.addr_bytes[5],
							vdev->vmdq_rx_q,
							vdev->tx_coreid,
							vdev->rx_coreid,
							rte_atomic64_read(&vdev->stats.rx_total_atomic),
							rte_atomic64_read(&vdev->stats.rx_success_atomic),
							vdev->stats.tx_total,
							vdev->stats.tx_success,
							vdev->stats.tx_tagged,
							vdev->stats.tx_dropped
				   );
		}
		RTE_LOG(INFO, VHOST_DATA, "=====  ======  ===================  =====  =======  ============  ============  ============  ============  ============  ============\n");
		// parsable version
		TAILQ_FOREACH(vdev, &vhost_dev_list, global_vdev_entry) {
			RTE_LOG(INFO, VHOST_DATA, "parsable-stats=%u-%u-%02x:%02x:%02x:%02x:%02x:%02x-%u-%u/%u-%"PRIu64"-%"PRIu64"-%"PRIu64"-%"PRIu64"-%"PRIu64"-%"PRIu64"\n",
							vdev->vid,
							vdev->vlan_tag,
							vdev->mac_address.addr_bytes[0],
							vdev->mac_address.addr_bytes[1],
							vdev->mac_address.addr_bytes[2],
							vdev->mac_address.addr_bytes[3],
							vdev->mac_address.addr_bytes[4],
							vdev->mac_address.addr_bytes[5],
							vdev->vmdq_rx_q,
							vdev->tx_coreid,
							vdev->rx_coreid,
							rte_atomic64_read(&vdev->stats.rx_total_atomic),
							rte_atomic64_read(&vdev->stats.rx_success_atomic),
							vdev->stats.tx_total,
							vdev->stats.tx_success,
							vdev->stats.tx_tagged,
							vdev->stats.tx_dropped
				   );
		}
}

/*
 * Builds up the correct configuration for VMDQ VLAN pool map
 * according to the pool & queue limits.
 */
static inline int
get_eth_conf(struct rte_eth_conf *eth_conf, uint32_t num_virtio_devices)
{
	struct rte_eth_vmdq_rx_conf conf;
	struct rte_eth_vmdq_rx_conf *def_conf = &vmdq_conf_default.rx_adv_conf.vmdq_rx_conf;
	unsigned i;

	memset(&conf, 0, sizeof(conf));
	conf.nb_queue_pools = (enum rte_eth_nb_pools) num_virtio_devices;
	conf.nb_pool_maps = num_virtio_devices;
	conf.enable_loop_back = def_conf->enable_loop_back;
	conf.rx_mode = def_conf->rx_mode;

	for (i = 0; i < conf.nb_pool_maps; i++) {
		/* Pool i corresponds to tag i */
		conf.pool_map[i].vlan_id = vlan_tags[i];
		conf.pool_map[i].pools = (1UL << i);
	}

	(void)(rte_memcpy(eth_conf, &vmdq_conf_default, sizeof(*eth_conf)));
	(void)(rte_memcpy(&eth_conf->rx_adv_conf.vmdq_rx_conf, &conf, sizeof(eth_conf->rx_adv_conf.vmdq_rx_conf)));
	return 0;
}

/*
 * Initialises a given port using global settings and with the rx buffers
 * coming from the mbuf_pool passed as parameter
 */
static inline int
port_init(uint16_t port)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_conf port_conf;
	struct rte_eth_rxconf *rxconf;
	struct rte_eth_txconf *txconf;
	int16_t rx_rings, tx_rings;
	uint16_t rx_ring_size, tx_ring_size;
	int retval;
	uint16_t q;

	/* The max pool number from dev_info will be used to validate the pool number specified in cmd line */
	rte_eth_dev_info_get(port, &dev_info);

	rxconf = &dev_info.default_rxconf;
	txconf = &dev_info.default_txconf;
	rxconf->rx_drop_en = 1;

	/*configure the number of supported virtio devices based on VMDQ limits */
	num_virtio_devices = dev_info.max_vmdq_pools;
	if(num_virtio_devices > MAX_VIRTIO_DEVICES)
		num_virtio_devices = MAX_VIRTIO_DEVICES;
	
	rx_ring_size = RTE_TEST_RX_DESC_DEFAULT;
	tx_ring_size = RTE_TEST_TX_DESC_DEFAULT;

	/*
	 * When dequeue zero copy is enabled, guest Tx used vring will be
	 * updated only when corresponding mbuf is freed. Thus, the nb_tx_desc
	 * (tx_ring_size here) must be small enough so that the driver will
	 * hit the free threshold easily and free mbufs timely. Otherwise,
	 * guest Tx vring would be starved.
	 */
	if (dequeue_zero_copy)
		tx_ring_size = 64;

	tx_rings = num_virtio_devices;

	/* Get port configuration. */
	retval = get_eth_conf(&port_conf, num_virtio_devices);
	if (retval < 0)
		return retval;
	/* NIC queues are divided into pf queues and vmdq queues.  */
	num_pf_queues = dev_info.max_rx_queues - dev_info.vmdq_queue_num;
	queues_per_pool = dev_info.vmdq_queue_num / dev_info.max_vmdq_pools;
	num_vmdq_queues = num_virtio_devices * queues_per_pool;
	num_queues = num_pf_queues + num_vmdq_queues;
	vmdq_queue_base = dev_info.vmdq_queue_base;
	vmdq_pool_base  = dev_info.vmdq_pool_base;
	RTE_LOG(INFO, VHOST_PORT, "pf queue num: %u, configured vmdq pool num: %u, each vmdq pool has %u queues\n",
		num_pf_queues, num_virtio_devices, queues_per_pool);

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	rx_rings = (uint16_t)dev_info.max_rx_queues;
	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
	/* Configure ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0) {
		RTE_LOG(ERR, VHOST_PORT, "Failed to configure port %u: %s.\n",
			port, strerror(-retval));
		return retval;
	}

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_ring_size, &tx_ring_size);
	if (retval != 0) {
		RTE_LOG(ERR, VHOST_PORT, "Failed to adjust number of descriptors for port %u: %s.\n", port, strerror(-retval));
		return retval;
	}
	if (rx_ring_size > RTE_TEST_RX_DESC_DEFAULT) {
		RTE_LOG(ERR, VHOST_PORT, "Mbuf pool has an insufficient size for Rx queues on port %u.\n", port);
		return -1;
	}

	/* Setup the queues. */
	rxconf->offloads = port_conf.rxmode.offloads;
	for (q = 0; q < rx_rings; q ++) {
		retval = rte_eth_rx_queue_setup(port, q, rx_ring_size, rte_eth_dev_socket_id(port), rxconf, mbuf_pool);
		if (retval < 0) {
			RTE_LOG(ERR, VHOST_PORT,
				"Failed to setup rx queue %u of port %u: %s.\n",
				q, port, strerror(-retval));
			return retval;
		}
	}
	txconf->offloads = port_conf.txmode.offloads;
	for (q = 0; q < tx_rings; q ++) {
		retval = rte_eth_tx_queue_setup(port, q, tx_ring_size,
						rte_eth_dev_socket_id(port),
						txconf);
		if (retval < 0) {
			RTE_LOG(ERR, VHOST_PORT,
				"Failed to setup tx queue %u of port %u: %s.\n",
				q, port, strerror(-retval));
			return retval;
		}
	}

	/* Start the device. */
	retval  = rte_eth_dev_start(port);
	if (retval < 0) {
		RTE_LOG(ERR, VHOST_PORT, "Failed to start port %u: %s\n",
			port, strerror(-retval));
		return retval;
	}

	if (promiscuous)
		rte_eth_promiscuous_enable(port);

	static struct rte_ether_addr vmdq_ports_eth_addr;
	rte_eth_macaddr_get(port, &vmdq_ports_eth_addr);
	RTE_LOG(INFO, VHOST_PORT, "Max virtio devices supported: %u\n", num_virtio_devices);
	RTE_LOG(INFO, VHOST_PORT, "Port %u MAC: %02"PRIx8" %02"PRIx8" %02"PRIx8
			" %02"PRIx8" %02"PRIx8" %02"PRIx8"\n",
			port,
			vmdq_ports_eth_addr.addr_bytes[0],
			vmdq_ports_eth_addr.addr_bytes[1],
			vmdq_ports_eth_addr.addr_bytes[2],
			vmdq_ports_eth_addr.addr_bytes[3],
			vmdq_ports_eth_addr.addr_bytes[4],
			vmdq_ports_eth_addr.addr_bytes[5]);

	return 0;
}

/*
 * Set socket file path.
 */
static int
us_vhost_parse_socket_path(const char *q_arg)
{
	char *old;

	/* parse number string */
	if (strnlen(q_arg, PATH_MAX) == PATH_MAX)
		return -1;

	old = socket_files;
	socket_files = realloc(socket_files, PATH_MAX * (nb_sockets + 1));
	if (socket_files == NULL) {
		free(old);
		return -1;
	}

	strlcpy(socket_files + nb_sockets * PATH_MAX, q_arg, PATH_MAX);
	nb_sockets++;

	return 0;
}

/*
 * Parse the port provided at run time.
 */
static int
parse_port(const char *port)
{
	char *end = NULL;
	unsigned long num;

	errno = 0;

	/* parse unsigned int string */
	num = strtoul(port, &end, 10);
	if ((port[0] == '\0') || (end == NULL) || (*end != '\0') || (errno != 0))
		return -1;

	return num;
}

/*
 * Parse num options at run time.
 */
static int
parse_num_opt(const char *q_arg, uint32_t max_valid_value)
{
	char *end = NULL;
	unsigned long num;

	errno = 0;

	/* parse unsigned int string */
	num = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0') || (errno != 0))
		return -1;

	if (num > max_valid_value)
		return -1;

	return num;
}

/*
 * Display usage
 */
static void
us_vhost_usage(const char *prgname)
{
	RTE_LOG(INFO, VHOST_CONFIG, "%s [EAL options] -- -p port_id\n"
	"		--socket-file <path>\n"
	"		-p port_id: to be used by application\n"
	"		--socket-file: The path of the socket file.\n"
	"		--tx-csum [0|1] disable/enable TX checksum offload.\n"
	"		--client register a vhost-user socket as client mode.\n"
	"		--dequeue-zero-copy enables dequeue zero copy\n",
	       prgname);
}

/*
 * Parse the arguments given in the command line of the application.
 */
static int
us_vhost_parse_args(int argc, char **argv)
{
	int opt, ret;
	int option_index;
	const char *prgname = argv[0];
	static struct option long_option[] = {
		{"socket-file", required_argument, NULL, 0},
		{"tx-csum", required_argument, NULL, 0},
		{"do_tag", required_argument, NULL, 1},
		{"do_shape", required_argument, NULL, 1},
		{"client", no_argument, &client_mode, 1},
		{"dequeue-zero-copy", no_argument, &dequeue_zero_copy, 1},
		{NULL, 0, 0, 0},
	};

	/* Parse command line */
	while ((opt = getopt_long(argc, argv, "p:P",
			long_option, &option_index)) != EOF) {
		switch (opt) {
		/* Port */
		case 'p':
			used_port_id = parse_port(optarg);
			if (used_port_id == -1) {
				RTE_LOG(INFO, VHOST_CONFIG, "Invalid port number\n");
				us_vhost_usage(prgname);
				return -1;
			}
			break;

		case 'P':
			promiscuous = 1;
			vmdq_conf_default.rx_adv_conf.vmdq_rx_conf.rx_mode = ETH_VMDQ_ACCEPT_BROADCAST | ETH_VMDQ_ACCEPT_MULTICAST;
			break;

		case 0:
			/* Enable/disable TX checksum offload. */
			if (!strncmp(long_option[option_index].name, "tx-csum", MAX_LONG_OPT_SZ)) {
				ret = parse_num_opt(optarg, 1);
				if (ret == -1) {
					RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for tx-csum [0|1]\n");
					us_vhost_usage(prgname);
					return -1;
				} else
					enable_tx_csum = ret;
			}
			
			/* Enable/disable tagging. */
			if (!strncmp(long_option[option_index].name, "do_tag", MAX_LONG_OPT_SZ)) {
				ret = parse_num_opt(optarg, 1);
				if (ret == -1) {
					RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for do_tag [0|1]\n");
					us_vhost_usage(prgname);
					return -1;
				} else
					do_tag = ret;
			}
			
			/* Enable/disable shaping. */
			if (!strncmp(long_option[option_index].name, "do_shape", MAX_LONG_OPT_SZ)) {
				ret = parse_num_opt(optarg, 1);
				if (ret == -1) {
					RTE_LOG(INFO, VHOST_CONFIG, "Invalid argument for do_shape [0|1]\n");
					us_vhost_usage(prgname);
					return -1;
				} else
					do_shape = ret;
			}

			/* Set socket file path. */
			if (!strncmp(long_option[option_index].name,
						"socket-file", MAX_LONG_OPT_SZ)) {
				if (us_vhost_parse_socket_path(optarg) == -1) {
					RTE_LOG(INFO, VHOST_CONFIG,
					"Invalid argument for socket name (Max %d characters)\n",
					PATH_MAX);
					us_vhost_usage(prgname);
					return -1;
				}
			}

			break;

			/* Invalid option - print options. */
		default:
			us_vhost_usage(prgname);
			return -1;
		}
	}

	return 0;
}

/*
 * This function learns the MAC address of the device and registers this along with a
 * vlan tag to a VMDq.
 */
static int
link_vmdq(struct vhost_dev *vdev, struct rte_mbuf *m)
{
	struct rte_ether_hdr *pkt_hdr;
	int i, ret, pool_id;

	/* Learn MAC address of guest device from packet */
	pkt_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

	for (i = 0; i < RTE_ETHER_ADDR_LEN; i++)
		vdev->mac_address.addr_bytes[i] = pkt_hdr->s_addr.addr_bytes[i];
	
	/* the pool id corresponds to the last MAC byte. */
	pool_id = GET_POOL_ID(vdev->mac_address);
	if(pools_used[pool_id]) {
		if (pool_allocation_failure == 0)
		{
			RTE_LOG(ERR, VHOST_DATA, "(%d) device uses a MAC address corresponding to a pool (%u) already allocated\n", vdev->vid, pool_id);
			pool_allocation_failure = 1;
		}
		return -1;
	}
	
	/* Configure RX pool and queue only if it's a data vHost */
	if(pool_id != -1) {
		vdev->vlan_tag = vlan_tags[pool_id];
		
		/* Assign pool queue to the device */
		vdev->pool_id = (uint16_t) pool_id;
		vdev->vmdq_rx_q = pool_id * queues_per_pool + vmdq_queue_base;
		/* Register the  MAC address to the pool of this device */
		ret = rte_eth_dev_mac_addr_add(used_port_id, &vdev->mac_address, pool_id + vmdq_pool_base);
		if (ret) {
			RTE_LOG(ERR, VHOST_DATA, "(%d) failed to add device MAC address to VMDQ\n", vdev->vid);
			return -1;
		}
		
		/* Enable VLAN stripping on the device receive queue */
		rte_eth_dev_set_vlan_strip_on_queue(used_port_id, vdev->vmdq_rx_q, 1);

		/* Set device as ready for RX */
		vdev->ready = DEVICE_DATA_RX;
		pools_used[pool_id] = 1;
	}
	else {
		/* Free the core which was assigned to RX, as it is not needed */
		lcore_info[vdev->rx_coreid].device_num--;
		TAILQ_REMOVE(&lcore_info[vdev->rx_coreid].rx_vdev_list, vdev, rx_lcore_vdev_entry);
		vdev->rx_coreid = 0; 
		vdev->ready = DEVICE_CONTROL;
	}
	
	return 0;
}

/*
 * Removes MAC address and VLAN tag from VMDq.
 * Ensures that nothing is adding buffers to the RX queue before disabling RX on the device.
 */
static inline void
unlink_vmdq(struct vhost_dev *vdev)
{
	unsigned i;
	unsigned rx_count;
	int pool_id;
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];

	if (vdev->ready == DEVICE_DATA_RX || vdev->ready == DEVICE_CONTROL) {
		pool_id = GET_POOL_ID(vdev->mac_address);

		/* Clear MAC and VLAN settings */
		rte_eth_dev_mac_addr_remove(used_port_id, &vdev->mac_address);
		for (i = 0; i < 6; i++)
			vdev->mac_address.addr_bytes[i] = 0;
		vdev->vlan_tag = 0;
		vdev->pool_id = 0;
			
		/* Clear out the receive buffers if it's a data vHost because the control
		 * channel has no receive buffer. */
		if(pool_id != -1) { // could have been vdev->ready == DEVICE_DATA_RX
			rx_count = rte_eth_rx_burst(used_port_id, (uint16_t)vdev->vmdq_rx_q, pkts_burst, MAX_PKT_BURST);

			while (rx_count) {
				for (i = 0; i < rx_count; i++)
					rte_pktmbuf_free(pkts_burst[i]);

				rx_count = rte_eth_rx_burst(used_port_id, (uint16_t)vdev->vmdq_rx_q, pkts_burst, MAX_PKT_BURST);
			}

			pools_used[pool_id] = 0;
		}
		
		vdev->ready = DEVICE_MAC_LEARNING;
	}
}

static inline void
free_pkts(struct rte_mbuf **pkts, uint16_t n)
{
	while (n--)
		rte_pktmbuf_free(pkts[n]);
}

static uint16_t
do_drain_mbuf_table(struct mbuf_table *tx_q)
{
	uint16_t count;

	count = rte_eth_tx_burst(used_port_id, tx_q->txq_id, tx_q->m_table, tx_q->len);
	if (unlikely(count < tx_q->len)) {
		free_pkts(&tx_q->m_table[count], tx_q->len - count);
	}
	tx_q->len = 0;
	return count;
}

static __rte_always_inline void
drain_eth_rx(struct vhost_dev *vdev)
{
	uint16_t rx_count, enqueue_count;
	struct rte_mbuf *pkts[MAX_PKT_BURST];

	/* Get data from NIC (and from the particular VMDq) */
	rx_count = rte_eth_rx_burst(used_port_id, vdev->vmdq_rx_q, pkts, MAX_PKT_BURST);
	if (!rx_count)
		return;
	
	/* Send to vHost */
	enqueue_count = rte_vhost_enqueue_burst(vdev->vid, VIRTIO_RXQ, pkts, rx_count);
	
	/* Update stats */
	rte_atomic64_add(&vdev->stats.rx_total_atomic, rx_count);
	rte_atomic64_add(&vdev->stats.rx_success_atomic, enqueue_count);

	/* Free memory used by packets */
	free_pkts(pkts, rx_count);
}
				
/**
 * Tag a packet based on the matching table.
 * Returns the number of tags added.
 */
static inline uint16_t tag_packet(struct rte_mbuf *packet, struct vhost_dev *vdev) {
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *tp_hdr;
	struct rte_ether_hdr *oh, *nh;
	
	/* We assume always Ethernet. */
	eth_hdr = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);

	/* Only IPv4: that means VLAN packets are not allowed. */
	if(eth_hdr->ether_type == BE_RTE_ETHER_TYPE_IPV4) {
		ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

		/* Only TCP/UDP. */
		if(ipv4_hdr->next_proto_id == IPPROTO_TCP || ipv4_hdr->next_proto_id == IPPROTO_UDP) {
			tp_hdr = (struct rte_udp_hdr *)((unsigned char *) ipv4_hdr + sizeof(struct rte_ipv4_hdr));
			/* Matching in the table. */
			for(int entry_id = 0; entry_id < N_ENTRIES_PER_VHOST; entry_id++) {
				/* Checking if source and destination IPs match. */
				if(memcmp(&matching_table[vdev->vlan_tag][entry_id].src_ip, &ipv4_hdr->src_addr, 8))
					continue;

				/* Checking if TP source and destination ports match */
				if(memcmp(&matching_table[vdev->vlan_tag][entry_id].src_port, &tp_hdr->src_port, 4))
					continue;

				if(matching_table[vdev->vlan_tag][entry_id].protocol != ipv4_hdr->next_proto_id)
					continue;

				/* Nothing to do */
				if(matching_table[vdev->vlan_tag][entry_id].n_tags == 0)
				    return 0;
				
				/* Shaping: if not allowed to send, do not tag it. */
				if(likely(do_shape)) 
				{
					uint64_t current_tsc;
					uint64_t generate_tokens;
					uint64_t delta_cycles;
					current_tsc = rte_rdtsc();
					// get difference in cycles					
					delta_cycles = current_tsc - matching_table[vdev->vlan_tag][entry_id].last_tsc;
					generate_tokens = delta_cycles * matching_table[vdev->vlan_tag][entry_id].rate_bps;
					// here we check for overflow, but we consume resources
					if ( delta_cycles != 0 && generate_tokens/delta_cycles != matching_table[vdev->vlan_tag][entry_id].rate_bps ) 
					{
						// we have overflow, which means a lot of time passed between two cycles, we just set generate tokens to big value
						// e.g., burst size
						generate_tokens = cpu_freq * matching_table[vdev->vlan_tag][entry_id].burst_bits;
					}
					
					// update timer
					matching_table[vdev->vlan_tag][entry_id].last_tsc = current_tsc;
                                	
					// add tokens
					if ((matching_table[vdev->vlan_tag][entry_id].n_tokens + generate_tokens) > cpu_freq * matching_table[vdev->vlan_tag][entry_id].burst_bits)
					{
						matching_table[vdev->vlan_tag][entry_id].n_tokens = cpu_freq * matching_table[vdev->vlan_tag][entry_id].burst_bits;
					} else
					{
						matching_table[vdev->vlan_tag][entry_id].n_tokens = matching_table[vdev->vlan_tag][entry_id].n_tokens + generate_tokens;
					}
			        	
					// Full packet size on line is: preamble size (8B) + eth. size (14B) + length of IP (variable) + CRC/FCS (4B) + inter. gap (12B) 
					uint64_t packet_size;
					packet_size = 8 + sizeof(struct rte_ether_hdr) + 4 + 12 + rte_bswap16(ipv4_hdr->total_length) + 4*matching_table[vdev->vlan_tag][entry_id].n_tags;
					
					// Check if we have enough tokens. *8 since packet size is in bytes.	
					if (matching_table[vdev->vlan_tag][entry_id].n_tokens >  8 * packet_size * cpu_freq)
					{
						matching_table[vdev->vlan_tag][entry_id].n_tokens -= 8 * packet_size * cpu_freq;
					} else
					{
						vdev->stats.tx_dropped++;
						return 0;
					}
				}
			
				/* We cannot tag if mbuf is shared */
				if (!RTE_MBUF_DIRECT(packet) || rte_mbuf_refcnt_read(packet) > 1) {
					return 0;
				}

				/* oh = old header, nh = new header */	
				oh = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);

				/* Make space in front */
				nh = (struct rte_ether_hdr*) rte_pktmbuf_prepend(packet, matching_table[vdev->vlan_tag][entry_id].n_tags * sizeof(struct rte_vlan_hdr));
				if (nh == NULL) {
					/* Not enough space */
					return 0;
				}

				/* Copy the (first part of) the Ethernet header at its new place (oh->nh) */
				memmove(nh, oh, 2 * RTE_ETHER_ADDR_LEN);

				/* Copy list of tags after source and destination MAC */
				rte_memcpy(&(nh->ether_type), matching_table[vdev->vlan_tag][entry_id].tags, matching_table[vdev->vlan_tag][entry_id].n_tags * 4);

				packet->ol_flags &= ~(PKT_RX_VLAN_STRIPPED | PKT_TX_VLAN);
				if (packet->ol_flags & PKT_TX_TUNNEL_MASK)
					packet->outer_l2_len += matching_table[vdev->vlan_tag][entry_id].n_tags * sizeof(struct rte_vlan_hdr);
				else
					packet->l2_len += matching_table[vdev->vlan_tag][entry_id].n_tags * sizeof(struct rte_vlan_hdr);
				return matching_table[vdev->vlan_tag][entry_id].n_tags;
			}

			return 0;
		}
	}

	return 0;
}

/**
 * Updates a matching table entry.
 */
static inline void update_table(struct rte_mbuf *packet) {
	/* Check that it is one of our ctrl packets */
	struct rte_ether_hdr *eth_hdr;
	eth_hdr = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
	
	/* Check if the frame has our Ether type */
	if(eth_hdr->ether_type == 0xbebe) {
		/* Skip Ethernet header and check data */
		uint8_t* data = (uint8_t*)(eth_hdr + 1);
		matching_table[data[0]][data[1]] = *((struct tagging_entry*) &data[2]); 
		/* we override last time stamp with the current one */
		matching_table[data[0]][data[1]].last_tsc = rte_rdtsc();
		/* in order to avoid using floats or doubles, number of tokens is multiplied with cpu_freq */ 
		matching_table[data[0]][data[1]].n_tokens = cpu_freq*matching_table[data[0]][data[1]].n_tokens;
	}
}

static __rte_always_inline void
drain_virtio_tx(struct vhost_dev *vdev)
{
	struct rte_mbuf *pkts[MAX_PKT_BURST];
	struct mbuf_table *tx_q = &lcore_tx_queue[rte_lcore_id()];
	uint16_t count;
	uint16_t i;
	uint8_t n_tags = 0;

	/* Get packets from vHost */
	count = rte_vhost_dequeue_burst(vdev->vid, VIRTIO_TXQ, mbuf_pool, pkts, MAX_PKT_BURST);

	/* setup VMDq for the first packet */
	if (unlikely(vdev->ready == DEVICE_MAC_LEARNING) && count) {
		if (vdev->remove || link_vmdq(vdev, pkts[0]) == -1)
			free_pkts(pkts, count);
	}

	/* Control processing */
	if(unlikely(vdev->ready == DEVICE_CONTROL)) {
		for (i = 0; i < count; ++i) {
			vdev->stats.tx_total += 1;
			update_table(pkts[i]);
			vdev->stats.tx_tagged += 1;
			rte_pktmbuf_free(pkts[i]);
		}
	}
	/* Data processing */
	else if(likely(vdev->ready == DEVICE_DATA_RX)) {
		for (i = 0; i < count; ++i) {
			vdev->stats.tx_total++;
			if(likely(do_tag)) {
				n_tags = tag_packet(pkts[i], vdev);			
				/* If packet tag packet returned zero tags, it means: */
				/* 1. Packet didn't match any rule in the table, */
		        	/* 2. Packet is maybe dropped by shaper, */
				/* 3. Other memory issues. */
				if (n_tags != 0) {
					/* Add packet to the TX queue */
					tx_q->m_table[tx_q->len++] = pkts[i];
					vdev->stats.tx_tagged++;
				} 
				else
				{
					/* Free pkt memory as we are dropping it. */
					rte_pktmbuf_free(pkts[i]);
				}
			}
			else
			{
				/* If we dont tag, we forward everything (?). */
				vdev->stats.tx_tagged++; 
				tx_q->m_table[tx_q->len++] = pkts[i];
			}

			/* Fully send buffer if it's full */
			if (unlikely(tx_q->len == MAX_PKT_BURST))
				vdev->stats.tx_success += (uint64_t)do_drain_mbuf_table(tx_q);

		}
		
		/* Drain table */	
		if(likely(tx_q->len > 0)) {
			vdev->stats.tx_success += (uint64_t)do_drain_mbuf_table(tx_q);
		}
	}
}

/*
 * Main function of vhost-switch. It basically does:
 *
 * for each vhost device {
 *    - drain_eth_rx()
 *
 *      Which drains the host eth Rx queue linked to the vhost device,
 *      and deliver all of them to guest virito Rx ring associated with
 *      this vhost device.
 *
 *    - drain_virtio_tx()
 *
 *      Which drains the guest virtio Tx queue and deliver all of them
 *      to the target, which could be another vhost device, or the
 *      physical eth dev.
 * }
 */
static int
switch_worker(void *arg __rte_unused)
{
	unsigned i;
	unsigned lcore_id = rte_lcore_id();
	struct vhost_dev *vdev;
	struct mbuf_table *tx_q;

	tx_q = &lcore_tx_queue[lcore_id];
	for (i = 0; i < rte_lcore_count(); i++) {
		if (lcore_ids[i] == lcore_id) {
			tx_q->txq_id = i;
			break;
		}
	}
	
	RTE_LOG(INFO, VHOST_DATA, "Processing started on core %u\n", lcore_id);
	cpu_freq = rte_get_tsc_hz();	

	while(1) {
		/* Inform the configuration core that we have exited the
		 * linked list and that no devices are in use if requested. */
		if (lcore_info[lcore_id].dev_removal_flag == REQUEST_DEV_REMOVAL)
			lcore_info[lcore_id].dev_removal_flag = ACK_DEV_REMOVAL;
 		
		/* Process each RX vhost device */
		TAILQ_FOREACH(vdev, &lcore_info[lcore_id].rx_vdev_list, rx_lcore_vdev_entry) {
			if (unlikely(vdev->remove)) {
				unlink_vmdq(vdev);
				continue;
			}

			/* control channel does not need to drain eth */
			if (likely(vdev->ready == DEVICE_DATA_RX))
				drain_eth_rx(vdev);
		}
		
		/* Process each TX vhost device */
		TAILQ_FOREACH(vdev, &lcore_info[lcore_id].tx_vdev_list, tx_lcore_vdev_entry) {
			drain_virtio_tx(vdev);
			if (unlikely(vdev->remove)) {
				vdev->ready = DEVICE_SAFE_REMOVE;
				continue;
			}
		}
	}

	return 0;
}

/*
 * Remove a device from the specific data core linked list and from the
 * main linked list.
 * Synchonization occurs through the use of the lcore dev_removal_flag.
 * Device is made volatile here to avoid re-ordering of dev->remove=1
 * which can cause an infinite loop in the rte_pause loop.
 */
static void
destroy_device(int vid)
{
	struct vhost_dev *vdev = NULL;
	int lcore;

	TAILQ_FOREACH(vdev, &vhost_dev_list, global_vdev_entry) {
		if (vdev->vid == vid)
			break;
	}
	if (!vdev)
		return;
	
	/* Set the remove flag and wait */
	vdev->remove = 1;
	while(vdev->ready != DEVICE_SAFE_REMOVE) {
		rte_pause();
	}

	print_stats();
	TAILQ_REMOVE(&lcore_info[vdev->tx_coreid].tx_vdev_list, vdev, tx_lcore_vdev_entry);
	TAILQ_REMOVE(&lcore_info[vdev->rx_coreid].rx_vdev_list, vdev, rx_lcore_vdev_entry);
	TAILQ_REMOVE(&vhost_dev_list, vdev, global_vdev_entry);


	/* Set the dev_removal_flag on each lcore */
	RTE_LCORE_FOREACH_SLAVE(lcore)
		lcore_info[lcore].dev_removal_flag = REQUEST_DEV_REMOVAL;

	/* Once each core has set the dev_removal_flag to ACK_DEV_REMOVAL
	 * we can be sure that they can no longer access the device removed
	 * from the linked lists and that the devices are no longer in use. */
	RTE_LCORE_FOREACH_SLAVE(lcore) {
		while (lcore_info[lcore].dev_removal_flag != ACK_DEV_REMOVAL)
			rte_pause();
	}

	lcore_info[vdev->tx_coreid].device_num--;
	lcore_info[vdev->rx_coreid].device_num--;

	RTE_LOG(INFO, VHOST_DATA, "(%d) device has been removed\n", vdev->vid);

	rte_free(vdev);
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 * 
 * The device is not yet assigned a RX queue. We have to wait for a first packet of the 
 * device to check its MAC address, and accordingly assign VLAN and hence queue.
 */
static int
new_device(int vid)
{
	int lcore, core_add = 0;
	uint32_t device_num_min = num_virtio_devices;
	struct vhost_dev *vdev;

	vdev = rte_zmalloc("vhost device", sizeof(*vdev), RTE_CACHE_LINE_SIZE);
	if (vdev == NULL) {
		RTE_LOG(INFO, VHOST_DATA, "(%d) couldn't allocate memory for vhost dev\n", vid);
		return -1;
	}
	
	vdev->vid = vid;

	TAILQ_INSERT_TAIL(&vhost_dev_list, vdev, global_vdev_entry);

	/* reset ready flag */
	vdev->ready = DEVICE_MAC_LEARNING;
	vdev->remove = 0;

	/* Find suitable lcores to add the device */
	
	/* For TX, use the same core for everyone: the first one */
	RTE_LCORE_FOREACH_SLAVE(lcore) {
		core_add = lcore;
		break;
	}
	vdev->tx_coreid = core_add;
	lcore_info[vdev->tx_coreid].device_num++;
	TAILQ_INSERT_TAIL(&lcore_info[vdev->tx_coreid].tx_vdev_list, vdev, tx_lcore_vdev_entry);
	
	/* For RX, balance the remaining ports among the devices */
	device_num_min = num_virtio_devices;
	RTE_LCORE_FOREACH_SLAVE(lcore) {
		/* Skip the TX core */
		if(lcore == vdev->tx_coreid)
			continue;
		if (lcore_info[lcore].device_num < device_num_min) {
			device_num_min = lcore_info[lcore].device_num;
			core_add = lcore;
		}
	}
	vdev->rx_coreid = core_add;
	lcore_info[vdev->rx_coreid].device_num++;
	TAILQ_INSERT_TAIL(&lcore_info[vdev->rx_coreid].rx_vdev_list, vdev, rx_lcore_vdev_entry);

	/* Disable notifications */
	rte_vhost_enable_guest_notification(vid, VIRTIO_RXQ, 0);
	rte_vhost_enable_guest_notification(vid, VIRTIO_TXQ, 0);

	RTE_LOG(INFO, VHOST_DATA, "(%d) device added: TX lcore %d, RX lcore %d\n", vid, vdev->tx_coreid, vdev->rx_coreid);

	return 0;
}

/*
 * These callback allow devices to be added to the data core when configuration
 * has been fully complete.
 */
static const struct vhost_device_ops virtio_net_device_ops =
{
	.new_device =  new_device,
	.destroy_device = destroy_device,
};

static void
unregister_drivers(int socket_num)
{
	int i, ret;

	for (i = 0; i < socket_num; i++) {
		ret = rte_vhost_driver_unregister(socket_files + i * PATH_MAX);
		if (ret != 0)
			RTE_LOG(ERR, VHOST_CONFIG, "Fail to unregister vhost driver for %s.\n", socket_files + i * PATH_MAX);
	}
}

/* Custom handling of signals to print/clear stats */
static void
signal_handler(int signum)
{
	/* When we receive a USR1 signal, print stats and table */
	if (signum == SIGUSR1) {
		print_table();
		print_stats();
	}

	/* When we receive a USR2 signal, reset stats */
	if (signum == SIGUSR2) {
		struct vhost_dev *vdev;
		TAILQ_FOREACH(vdev, &vhost_dev_list, global_vdev_entry) {
			memset(&vdev->stats, 0, sizeof(struct device_statistics));
		}	
	
		RTE_LOG(INFO, VHOST_DATA, "** Statistics have been reset **\n");
		return;
	}

	/* When we receive a RTMIN or SIGINT signal, stop all drivers */
	if (signum == SIGRTMIN || signum == SIGINT) {
		unregister_drivers(nb_sockets);
		return;
	}
}


/*
 * Main function, does initialisation and calls the per-lcore functions.
 */
int
main(int argc, char *argv[])
{
	unsigned lcore_id, core_id = 0;
	unsigned nb_ports;
	int ret, i;
	uint16_t portid;
	uint64_t flags = 0;

	/* Associate signal_hanlder function with signals */
	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);
	signal(SIGRTMIN, signal_handler);
	signal(SIGINT, signal_handler);

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	argc -= ret;
	argv += ret;

	/* Parse app arguments */
	ret = us_vhost_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid argument\n");

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		TAILQ_INIT(&lcore_info[lcore_id].rx_vdev_list);
		TAILQ_INIT(&lcore_info[lcore_id].tx_vdev_list);

		if (rte_lcore_is_enabled(lcore_id))
			lcore_ids[core_id++] = lcore_id;
	}

	if (rte_lcore_count() > RTE_MAX_LCORE)
		rte_exit(EXIT_FAILURE,"Not enough cores\n");

	/* Get the number of physical ports */
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports != 1) {
		RTE_LOG(INFO, VHOST_PORT, "%u ports are enabled, but exactly 1 port should be enabled\n", nb_ports);
		return -1;
	}

	if (!rte_eth_dev_is_valid_port(used_port_id)) {
		RTE_LOG(INFO, VHOST_PORT, "The port ID %u to use is invalid\n", used_port_id);
		return -1;
	}

	/*
	 * - Each rx queue would reserve @nr_rx_desc mbufs at queue setup stage
	 * - For each switch core (A CPU core does the packet switch), we need
	 *   also make some reservation for receiving the packets from virtio
	 *   Tx queue. How many is enough depends on the usage. It's normally
	 *   a simple calculation like following:
	 *       MAX_PKT_BURST * max packet size / mbuf size
	 * - Similarly, for each switching core, we should serve @nr_rx_desc
	 *   mbufs for receiving the packets from physical NIC device.
	 * - We also need make sure, for each switch core, we have allocated
	 *   enough mbufs to fill up the mbuf cache.
	 */
	uint32_t nr_mbufs;
	uint32_t nr_mbufs_per_core;
	uint32_t mtu = 1500;

	nr_mbufs_per_core  = (mtu + RTE_MBUF_DEFAULT_BUF_SIZE) * MAX_PKT_BURST / (RTE_MBUF_DEFAULT_BUF_SIZE - RTE_PKTMBUF_HEADROOM);
	nr_mbufs_per_core += RTE_TEST_RX_DESC_DEFAULT;

	nr_mbufs  = MAX_VIRTIO_DEVICES * RTE_TEST_RX_DESC_DEFAULT * 2;
	nr_mbufs += nr_mbufs_per_core * (rte_lcore_count() - 1);

	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", nr_mbufs, 128, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Enable VT loop back to let NIC send back packets sent by guests to other guests */
	vmdq_conf_default.rx_adv_conf.vmdq_rx_conf.enable_loop_back = 1;
	RTE_LOG(DEBUG, VHOST_CONFIG, "Enable loop back for L2 switch in vmdq.\n");

	/* Initialize all ports */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if (used_port_id != portid) {
			RTE_LOG(INFO, VHOST_PORT, "Skipping disabled port %d\n", portid);
			continue;
		}
		
		if (port_init(portid) != 0)
			rte_exit(EXIT_FAILURE, "Cannot initialize network ports\n");
	}

	/* Launch all data cores */
	RTE_LCORE_FOREACH_SLAVE(lcore_id)
		rte_eal_remote_launch(switch_worker, NULL, lcore_id);

	/* Register vhost user driver to handle vhost messages. */
	if (client_mode)
		flags |= RTE_VHOST_USER_CLIENT;
	if (dequeue_zero_copy)
		flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;
	for (i = 0; i < nb_sockets; i++) {
		char *file = socket_files + i * PATH_MAX;
		ret = rte_vhost_driver_register(file, flags);
		if (ret != 0) {
			unregister_drivers(i);
			rte_exit(EXIT_FAILURE, "vhost driver register failure\n");
		}

		rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_MRG_RXBUF);

		if (enable_tx_csum == 0) {
			rte_vhost_driver_disable_features(file, 1ULL << VIRTIO_NET_F_CSUM);
		}

		if (promiscuous) {
			rte_vhost_driver_enable_features(file, 1ULL << VIRTIO_NET_F_CTRL_RX);
		}

		ret = rte_vhost_driver_callback_register(file, &virtio_net_device_ops);
		if (ret != 0) {
			rte_exit(EXIT_FAILURE, "failed to register vhost driver callbacks.\n");
		}

		if (rte_vhost_driver_start(file) < 0) {
			rte_exit(EXIT_FAILURE, "failed to start vhost driver.\n");
		}
	}

	RTE_LCORE_FOREACH_SLAVE(lcore_id)
		rte_eal_wait_lcore(lcore_id);

	return 0;

}
