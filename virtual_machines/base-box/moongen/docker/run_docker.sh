#!/bin/bash

# Script that starts MoonGen in a docker
# container.
# 
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

docker rm moongen

docker run -d --privileged \
	-v /sys/bus/pci/drivers:/sys/bus/pci/drivers \
	-v /sys/kernel/mm/hugepages:/sys/kernel/mm/hugepages \
	-v /sys/devices/system/node:/sys/devices/system/node \
	-v /mnt/huge:/mnt/huge \
	-v /lib/modules:/lib/modules \
	-v /dev:/dev \
	--net="host" \
	--name moongen \
	-ti docker_moongen \
	$@
