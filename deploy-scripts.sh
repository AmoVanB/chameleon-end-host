#!/bin/bash

# This script deploys the Chameleon script to the /usr/bin/
# directory.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

rm -rf /usr/bin/start-dpdk-tagging
ln -s $(pwd)/virtual_switch/start-dpdk-tagging.sh /usr/bin/start-dpdk-tagging

rm -rf /usr/bin/stop-dpdk-tagging
ln -s $(pwd)/virtual_switch/stop-dpdk-tagging.sh /usr/bin/stop-dpdk-tagging

rm -rf /usr/bin/create-vm
ln -s $(pwd)/virtual_machines/create-vm.sh /usr/bin/create-vm

rm -rf /usr/bin/delete-vm
ln -s $(pwd)/virtual_machines/delete-vm.sh /usr/bin/delete-vm

rm -rf /usr/bin/list-vms
ln -s $(pwd)/virtual_machines/list-vms.sh /usr/bin/list-vms

rm -rf /usr/bin/create-base-box
ln -s $(pwd)/virtual_machines/create-base-box.sh /usr/bin/create-base-box

rm -rf /usr/bin/send-lldp
ln -s $(pwd)/topology_discovery/send-lldp.py /usr/bin/send-lldp

