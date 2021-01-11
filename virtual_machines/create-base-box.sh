#!/bin/bash

# This script creates a base vagrant box with all the 
# software needed for the different VMs.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

# Go to base-box dir
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cd $DIR/base-box/

# Make sure the huge pages are there
if [ ! -d /mnt/huge ]; then
	echo "Huges pages seem not to be available!"
	exit -1
fi

# Make sure we can use the hugepages
chmod -R 777 /mnt/huge
if [ $? != 0 ]; then
	echo "Impossible to set permissions for the hugepages"
	exit -1
fi

# Create base VM and turn it off
vagrant destroy -f
vagrant box update
vagrant up
vagrant halt

echo "Packaging base box"
rm -f chameleon-base-vm.box
# Before packaging, ensure that libvirt uses the proper virt-sysprep options
sed -i "s/virt-sysprep.*/virt-sysprep --no-logfile --operations defaults,-ssh-userdir,-ssh-hostkeys,-customize -a #{@tmp_img}\`/" /usr/share/rubygems-integration/all/gems/vagrant-libvirt-*/lib/vagrant-libvirt/action/package_domain.rb
vagrant package --output chameleon-base-vm.box

# Delete previously created box to ensure we don't use outdated stuff
vagrant box remove chameleon/base
for vol in $(virsh vol-list default | grep .img | cut -d " " -f 2 | grep chameleon); do
	virsh vol-delete --pool default $vol
done

# Add new box to store
vagrant box add chameleon-base-vm.box --name chameleon/base

# Restore original dir
cd -
