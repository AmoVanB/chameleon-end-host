#!/bin/bash

# Startup script for all VMs.
# 
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

# Up the main interface
ip link set dev eth1 up

# If control VM, install the update script
if hostname | grep -qE "00$"; then
	chmod +x /home/vagrant/update-matching-table.py
	ln -s /home/vagrant/update-matching-table.py /usr/bin/update-matching-table
fi

# Allocate huge pages
mkdir -p /mnt/huge
echo 150 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
mount -t hugetlbfs nodev /mnt/huge

# Sleep to make sure interface is correctly up
sleep 3

# Send MAC advertisement 
chmod +x /home/vagrant/send-mac-advertisement.py
/home/vagrant/send-mac-advertisement.py
