#!/usr/bin/python3

"""
This script sends an LLDP packet advertising a given IP (first argument) on a
given port (second argument).

Author: Amaury Van Bemten <amaury.van-bemten@tum.de>
"""

from scapy.all import *
import sys

if len(sys.argv) < 3:
    print("Missing args: IP to advertise and interface to send to")
    sys.exit(-1)

ip = sys.argv[1]
ifc = sys.argv[2]

chassis = bytearray(7)
chassis[0:3] = (0x02, 0x05, 0x05)
chassis[3:] = (int(ip.split(".")[0]), int(ip.split(".")[1]), int(ip.split(".")[2]), int(ip.split(".")[3])) # ip address
port = bytearray(5)
port[0:3] = (0x04, 0x03, 0x07)
port[3:] = (0, 0) # port number
TTL = bytearray( (0x06,0x02, 0x00,0x78) )
end = bytearray( (0x00, 0x00) )
payload = bytes( chassis + port + TTL + end )
mac_lldp_multicast = '01:80:c2:00:00:0e'
eth = Ether(dst=mac_lldp_multicast, type=0x88cc)
# Length is 14 + 18 = 32, we need to padd to 60: 28 bytes
frame = eth / Raw(load=bytes(payload)) / Padding(b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00')
frame.show()
sendp(frame, iface=ifc)
