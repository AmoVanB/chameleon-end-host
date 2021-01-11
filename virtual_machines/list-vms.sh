#!/bin/bash

# This script lists all the vagrant VMs.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

if [ "$EUID" -ne 0 ]; then
	echo "This script must run as root"
	exit -1
fi

echo "Folders: "
ls /vagrant/

virsh list --all
