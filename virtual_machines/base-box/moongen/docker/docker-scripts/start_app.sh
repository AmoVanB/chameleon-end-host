#!/bin/bash

# Script to start MoonGen.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

if [ $# -lt 1 ]; then
	echo "Usage: $0 lua_script [args...]"
	exit -1
fi

script=$1

if [ "$script" == "l3-load-with-bursts" ]; then
	if [ $# -lt 6 ]; then
		echo "Usage: $0 $1 packet_size duration n_flows burst_size inter_burst"
		exit -1
	fi
else
	echo "Only the l3-load-with-bursts.lua scripts are known!"
fi

# The port the app should use
PORT=0000:00:06.0

# Make sure the port is bound to the correct driver
modprobe uio_pci_generic
./MoonGen/libmoon/deps/dpdk/usertools/dpdk-devbind.py --unbind $PORT
./MoonGen/libmoon/deps/dpdk/usertools/dpdk-devbind.py --bind=uio_pci_generic $PORT
if [ $? != 0 ]; then
	echo "Impossible to bind $PORT to DPDK driver!"
	exit -1
fi

./MoonGen/build/MoonGen /root/moongen-scripts/$1.lua 0 --size $2 --duration $3 --flows $4 --burst $5 --delay $6
