#!/bin/bash

# This script starts the Chameleon virtual switch docker container.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

hostname=$(hostname)
if [[ "$hostname" == "hazard" ]] || [[ "$hostname" == "rooney" ]] || [[ "$hostname" == "daei" ]] || [[ "$hostname" == "schweinsteiger" ]] || [[ "$hostname" == "kahn" ]] || [[ "$hostname" == "ballack" ]]; then
	# These servers have 128GB RAM, hence 64GB per NUMA node, hence 60 huge pages per NUMA node for VMs
	n_huge_pages=60
else
	# other servers have 64GB RAM, hence 32GB per NUMA node, hence 30 huge pages per NUMA node for VMs
	n_huge_pages=30
fi
echo $n_huge_pages > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
echo $n_huge_pages > /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages
mkdir -p /mnt/huge
umount /mnt/huge
mount -t hugetlbfs -o pagesize=1G nodev /mnt/huge

# Set up LLC
apt-get update -y && apt-get install intel-cmt-cat
modprobe msr
if [[ "$hostname" == "daei" ]] || [[ "$hostname" == "schweinsteiger" ]] || [[ "$hostname" == "kahn" ]] || [[ "$hostname" == "ballack" ]]; then
	# Non-Dell
	pqos -e "llc@0:0=0x0f0;llc@0:1=0x00f;llc@0:2=0x700"
	pqos -a "llc:1=16;llc:2=18"
else
	# Dell
	pqos -e "llc@0:0=0xff000;llc@0:1=0x000ff;llc@0:2=0x00f00"
	pqos -a "llc:1=16;llc:2=18"
fi

docker rm dpdk
docker run -it -d --privileged \
	-v /sys/bus/pci/drivers:/sys/bus/pci/drivers \
	-v /sys/kernel/mm/hugepages:/sys/kernel/mm/hugepages \
	-v /sys/devices/system/node:/sys/devices/system/node \
	-v /mnt/huge:/mnt/huge \
	-v /lib/modules:/lib/modules \
	-v /dev:/dev \
	-v /tmp:/tmp \
	--net="host" \
	--name dpdk \
	-ti docker_dpdk \
	$1

