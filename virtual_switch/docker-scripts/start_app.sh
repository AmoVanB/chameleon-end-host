#!/bin/bash

# This script starts the DPDK app that implements the Chameleon
# virtual switch.
# 
# Note: the 3 cores on which the app should run are hardcoded in
# this script. Change at will. Ideally, these 3 cores should
# also be passed to the 'isolcpus' kernel parameter to ensure
# that no other process uses them.
# 
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

# Load variables
SCRIPT=$(readlink -f $0)
SCRIPTPATH=$(dirname $SCRIPT)
. $SCRIPTPATH/dpdk_profile.sh

# The port the app should use
PORT=$1

# Activate driver
modprobe uio_pci_generic

# Make sure the two ports are bound to the correct driver
$RTE_SDK/usertools/dpdk-devbind.py --unbind $PORT
$RTE_SDK/usertools/dpdk-devbind.py --bind=uio_pci_generic $PORT
if [ $? != 0 ]; then
	echo "Impossible to bind $PORT to DPDK driver!"
	exit -1
fi
 
# root@hazard:~/dpdk-stable/usertools# ./cpu_layout.py 
# ======================================================================
# Core and Socket Information (as reported by '/sys/devices/system/cpu')
# ======================================================================
#  
# cores =  [0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13]
# sockets =  [0, 1]
#
#         Socket 0    Socket 1   
#         --------    --------   
# Core 0  [0]         [1]        
# Core 1  [2]         [3]        
# Core 2  [4]         [5]        
# Core 3  [6]         [7]        
# Core 4  [8]         [9]        
# Core 5  [10]        [11]       
# Core 8  [12]        [13]       
# Core 9  [14]        [15]       
# Core 10 [16]        [17]       
# Core 11 [18]        [19]       
# Core 12 [20]        [21]       
# Core 13 [22]        [23]
#
# The NIC is on socket 0:
# root@hazard:/# cat /sys/class/net/enp4s0f0/device/local_cpulist
# 0,2,4,6,8,10,12,14,16,18,20,22
# root@hazard:/# cat /sys/class/net/enp4s0f1/device/local_cpulist
# 0,2,4,6,8,10,12,14,16,18,20,22
#
# -l: Use ports 14, 16, 18 for DPDK. They are on the same socket as the NIC and
#     different cores.
# Note that we use the kernel parameter "isolcpus" to prevent the kernel from using
# lcores 14,16,18 to ensure our DPDK threads are not bothered.

./app/build/dpdk-tagging -l 14,16,18 -n 4 --log-level 8 --socket-mem 1024 -- --socket-file /tmp/sock0 -p 0 

# Connect the interfaces back to the kernel
$RTE_SDK/usertools/dpdk-devbind.py --bind=ixgbe $PORT
