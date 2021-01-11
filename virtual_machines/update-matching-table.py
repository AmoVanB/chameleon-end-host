#!/usr/bin/python3

"""
This script, to be used by VM 0, sends a configuration
message to the virtual switch to create a particular
tagging and shaping rule.

Author: Amaury Van Bemten <amaury.van-bemten@tum.de>
"""

from scapy.all import *
import sys

# import scapy config
from scapy.all import conf as scapyconf
# disable scapy promiscuous mode since it is already in this mode
scapyconf.sniff_promisc = 0

def update_matching_rule(kni_id, rule_id, protocol, source_ip, destination_ip, source_port, destination_port, tags, rate_bps, burst_bits):
    payload = list(kni_id.to_bytes(1, byteorder = 'big'))
    payload += list(rule_id.to_bytes(1, byteorder = 'big'))
    payload += list(protocol.to_bytes(1, byteorder = 'big'))
    payload += list(int(0).to_bytes(3, byteorder = 'big'))
    if len(source_ip) != 4 or len(destination_ip) != 4:
        print("Source and destination IPs should be arrays of size 4")
        sys.exit(-1)
    for ip_elem in list(source_ip) + list(destination_ip):
        payload += list(ip_elem.to_bytes(1, byteorder = 'big'))
    payload += list(source_port.to_bytes(2, byteorder = 'big'))
    payload += list(destination_port.to_bytes(2, byteorder = 'big'))
    payload += list(rate_bps.to_bytes(8, byteorder = 'little')) # rate, in bits per second  
    payload += list(burst_bits.to_bytes(8, byteorder = 'little')) # burst, in bits
    n_tokens = burst_bits
    rte_timestamp = int(10000)
    payload += list(n_tokens.to_bytes(8, byteorder = 'little')) # n_tokens, should be initially the same as burst, but later on it is converted to burst*cpu_freq
    payload += list(rte_timestamp.to_bytes(8, byteorder = 'little')) # timestamp, it will be overwritten anyway
    payload += list(len(tags).to_bytes(2, byteorder = 'little'))
    for tag in tags:
        payload += list(0x8100.to_bytes(2, byteorder = 'big'))
        payload += list(tag.to_bytes(2, byteorder = 'big'))

    frame = Ether(type=0xbebe) / Raw(payload)
    frame.show()
    sendp(frame, iface="eth1")

def clean_table():
    for kni_id in range(0, 20):
        for rule_id in range(0, 5):
            update_matching_rule(kni_id, rule_id, 0, [0, 0, 0, 0], [0, 0, 0, 0], 0, 0, [0, 0, 0, 0, 0])

if len(sys.argv) < 11:
    print("Need at least 10 parameters")
    sys.exit(-1)

kni_id = int(sys.argv[1])
rule_id = int(sys.argv[2])
protocol = int(sys.argv[3])
source_ip = [int(elem) for elem in sys.argv[4].split(".")]
destination_ip = [int(elem) for elem in sys.argv[5].split(".")]
source_port = int(sys.argv[6])
destination_port = int(sys.argv[7])
tags = [int(elem) for elem in sys.argv[8].split(",")]
rate_bps = int(sys.argv[9])
burst_bits = int(sys.argv[10])

if(len(tags) > 10):
    print("At most 10 tags are allowed in the current implementation")
    sys.exit(-1)

update_matching_rule(kni_id, rule_id, protocol, source_ip, destination_ip, source_port, destination_port, tags, rate_bps, burst_bits)
