#!/bin/bash

# This script installs the dependencies needed to run the end host code
# of the Chameleon networking system.
#
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

# install perl and python3
apt-get -y install perl python3 python3-pip

# install scapy
pip3 install scapy

# install docker
apt-get -y install apt-transport-https ca-certificates curl gnupg-agent software-properties-common
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | apt-key add -
add-apt-repository "deb [arch=amd64] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable"
apt-get update
apt-get -y install docker-ce

# install vagrant
apt-get install -y qemu-kvm libvirt-bin
apt-get install -y vagrant
mkdir -p /vagrant/
