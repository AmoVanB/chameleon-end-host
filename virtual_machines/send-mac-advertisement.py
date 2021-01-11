#!/usr/bin/python3

"""
This script sends a dummy L2 packet on eth1 to
advertise the MAC address and virtio address of a 
VM to the virtual switch.

Author: Amaury Van Bemten <amaury.van-bemten@tum.de>
"""

from scapy.all import *
import netifaces
src_mac = netifaces.ifaddresses('eth1')[netifaces.AF_LINK][0]['addr']
dst_mac = "ff:ff:ff:ff:ff:ff"
frame = Ether(src=src_mac, dst=dst_mac)
frame.show()
sendp(frame, iface="eth1")
