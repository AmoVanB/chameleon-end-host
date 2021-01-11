#!/bin/bash

# This script installs the required software (scapy,
# moongen, and docker) and SSH keys in the vagrant
# base box.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

# Configure DNS
echo "nameserver 8.8.8.8" > /etc/resolv.conf

# Install scapy (through pip because python3-scapy is too old)
apt-get update
apt-get install -y python3-pip
pip3 install scapy

# Add ssh key to root
cat /dev/zero | ssh-keygen -q -N ""
cat /home/vagrant/host_public_key.pub >> /home/vagrant/.ssh/authorized_keys
cat /home/vagrant/host_public_key.pub >> /root/.ssh/authorized_keys
# Add your SSH keys here
echo "ssh-rsa XXX" >> /root/.ssh/authorized_keys
echo "ssh-rsa YYY" >> /root/.ssh/authorized_keys

# Move moongen code
cp -r /home/vagrant/moongen /root/moongen

# Install docker
chmod +x /root/moongen/install-docker.sh
/root/moongen/install-docker.sh

# Build MoonGen
cd /root/moongen/docker/
chmod +x build_docker.sh
./build_docker.sh
