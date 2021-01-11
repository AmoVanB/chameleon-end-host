#!/bin/bash

# This script starts the Chameleon virtual switch on three 
# servers, creates VMs on all of them, configures the
# matching table of the virtual switches and generates the
# following traffic:
# PTP:
# FIRST_SERVER -> SECOND_SERVER (slave -> master)
# MoonGen traffic:
# FIRST_SERVER -> SECOND_SERVER
# THIRD_SERVER -> SECOND_SERVER
# 
# Note: the insert_tagging_rules function depends on the
# network infrastructure and switches configuration. The
# method here ensures the appropriate tags are configured
# for the flows to reach their destination in our specific
# testbed network.
# The pushed tags must be updated to match the configuration
# of the switches in your network.
# 
# Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

FIRST_SERVER=kane.forschung.lkn.ei.tum.de
SECOND_SERVER=gerrard.forschung.lkn.ei.tum.de
THIRD_SERVER=rooney.forschung.lkn.ei.tum.de
START_TAGGING="start-dpdk-tagging 0000:04:00.0"
STOP_TAGGING="stop-dpdk-tagging 0000:04:00.0"

function ssh_no_key() {
	ssh -o StrictHostKeyChecking=no -o LogLevel=QUIET $@
}

function scp_no_key() {
	scp -q -o StrictHostKeyChecking=no -o LogLevel=QUIET $@
}

function delete_running_vms() {
	echo "Deleting VMs..."
	ssh_no_key root@$FIRST_SERVER  "delete-vm 0 && delete-vm 1 && delete-vm 2" > /dev/null
	ssh_no_key root@$SECOND_SERVER "delete-vm 0 && delete-vm 1" > /dev/null
	ssh_no_key root@$THIRD_SERVER  "delete-vm 0 && delete-vm 1" > /dev/null
}

function stop_dpdk_tagging() {
	echo "Stopping DPDK..."
	ssh_no_key root@$FIRST_SERVER  "$STOP_TAGGING" > /dev/null &
	ssh_no_key root@$SECOND_SERVER "$STOP_TAGGING" > /dev/null &
	ssh_no_key root@$THIRD_SERVER  "$STOP_TAGGING" > /dev/null &
	wait
}

function start_dpdk_tagging() {
	echo "Starting DPDK..."
	ssh_no_key root@$FIRST_SERVER  "$START_TAGGING" > /dev/null &
	ssh_no_key root@$SECOND_SERVER "$START_TAGGING" > /dev/null &
	ssh_no_key root@$THIRD_SERVER  "$START_TAGGING" > /dev/null &
	wait
}

function create_base_box() {
	echo "Creating base box..."
	ssh_no_key root@$FIRST_SERVER  "create-base-box" > /dev/null &
	ssh_no_key root@$SECOND_SERVER "create-base-box" > /dev/null &
	ssh_no_key root@$THIRD_SERVER  "create-base-box" > /dev/null &
}

function create_vms() {
	echo "Creating the VMs..."
	ssh_no_key root@$FIRST_SERVER  "create-vm 0 && create-vm 1 && create-vm 2" > /dev/null &
	ssh_no_key root@$SECOND_SERVER "create-vm 0 && create-vm 1" > /dev/null &
	ssh_no_key root@$THIRD_SERVER  "create-vm 0 && create-vm 1" > /dev/null &
	wait
}

function turn_off_vms() {
	echo "Turning off the VMs..."
	for vm in $(seq 0 1); do
		ssh_no_key root@$FIRST_SERVER "cd /vagrant/$vm && vagrant halt" > /dev/null &
		ssh_no_key root@$SECOND_SERVER "cd /vagrant/$vm && vagrant halt" > /dev/null &
		ssh_no_key root@$THIRD_SERVER "cd /vagrant/$vm && vagrant halt" > /dev/null &
	done
	ssh_no_key root@$FIRST_SERVER "cd /vagrant/2 && vagrant halt" > /dev/null &
	wait
}

function start_vms() {
	echo "Starting the VMs..."
	ssh_no_key root@$FIRST_SERVER  "chmod -R 777 /mnt/huge ; chmod ugo+rwx /tmp/sock0" > /dev/null &
	ssh_no_key root@$SECOND_SERVER "chmod -R 777 /mnt/huge ; chmod ugo+rwx /tmp/sock0" > /dev/null &
	ssh_no_key root@$THIRD_SERVER  "chmod -R 777 /mnt/huge ; chmod ugo+rwx /tmp/sock0" > /dev/null &
	for vm in $(seq 0 1); do
		ssh_no_key root@$FIRST_SERVER  "cd /vagrant/$vm && vagrant up" > /dev/null &
		ssh_no_key root@$SECOND_SERVER "cd /vagrant/$vm && vagrant up" > /dev/null &
		ssh_no_key root@$THIRD_SERVER  "cd /vagrant/$vm && vagrant up" > /dev/null &
		wait
	done
	ssh_no_key root@$FIRST_SERVER "cd /vagrant/2 && vagrant up" > /dev/null
}

function build_moongen_on_vms() {
	echo "Building MoonGen on VMs..."
	ssh_no_key -p 20001 root@$FIRST_SERVER  "cd /root/moongen/docker/ && ./build_docker.sh" > /dev/null &
	ssh_no_key -p 20002 root@$FIRST_SERVER "cd /root/moongen/docker/ && ./build_docker.sh" > /dev/null &
	ssh_no_key -p 20001 root@$SECOND_SERVER "cd /root/moongen/docker/ && ./build_docker.sh" > /dev/null &
	ssh_no_key -p 20001 root@$THIRD_SERVER  "cd /root/moongen/docker/ && ./build_docker.sh" > /dev/null &
	wait
}

function configure_moongen_on_vms() {
	echo "Configuring MoonGen on VMs..."
	ssh_no_key -p 20002 root@$FIRST_SERVER "cd /root/moongen/docker/moongen-scripts && sed -i \"s/.*local SRC_MAC.*=.*/local SRC_MAC=\\\"02:ca:4e:00:00:02\\\"/\" l3-load-with-bursts.lua && sed -i \"s/.*local DST_MAC.*=.*/local DST_MAC=\\\"02:9e:66:a6:d0:01\\\"/\" l3-load-with-bursts.lua" > /dev/null &
	ssh_no_key -p 20001 root@$THIRD_SERVER "cd /root/moongen/docker/moongen-scripts && sed -i \"s/.*local SRC_MAC.*=.*/local SRC_MAC=\\\"02:60:02:e1:00:01\\\"/\" l3-load-with-bursts.lua && sed -i \"s/.*local DST_MAC.*=.*/local DST_MAC=\\\"02:9e:66:a6:d0:01\\\"/\" l3-load-with-bursts.lua && sed -i \"s/.*local SRC_IP_BASE.*=.*/local SRC_IP_BASE=\\\"10.0.0.2\\\"/\" l3-load-with-bursts.lua && sed -i \"s/.*local DST_IP.*=.*/local DST_IP=\\\"20.0.0.2\\\"/\" l3-load-with-bursts.lua" > /dev/null &
	wait
}

function insert_tagging_rules() {
	echo "Inserting tagging rules for VMs..."
	ssh_no_key -p 20000 root@$FIRST_SERVER  "update-matching-table 1 0 17 30.0.0.1 30.0.0.2 319  319  540,1 1000000000 800000" > /dev/null
	ssh_no_key -p 20000 root@$FIRST_SERVER  "update-matching-table 1 1 17 30.0.0.1 30.0.0.2 320  320  540,1 1000000000 800000" > /dev/null &
	ssh_no_key -p 20000 root@$SECOND_SERVER "update-matching-table 1 0 17 30.0.0.2 30.0.0.1 319  319  530,1 1000000000 800000" > /dev/null
	ssh_no_key -p 20000 root@$SECOND_SERVER "update-matching-table 1 1 17 30.0.0.2 30.0.0.1 320  320  530,1 1000000000 800000" > /dev/null &
  # for our switches, 541 is a high priority queue
  queue=541
	ssh_no_key -p 20000 root@$THIRD_SERVER  "update-matching-table 1 0 17 10.0.0.2 20.0.0.2 5000 6000 550,540,$queue,1 10000000000 800000" > /dev/null &
	ssh_no_key -p 20000 root@$FIRST_SERVER  "update-matching-table 2 0 17 10.0.0.1 20.0.0.1 5000 6000 $queue,1 10000000000 800000" > /dev/null &
	wait
}

clear_stats() {
	echo "Clearing stats..."
	ssh_no_key -p 20001 root@$FIRST_SERVER "rm -f ~/stats-file.log" &
	ssh_no_key -p 20001 root@$FIRST_SERVER "rm -f ~/log-file.log" &
	ssh_no_key root@$FIRST_SERVER "pkill -USR2 dpdk-tagging" &

	ssh_no_key -p 20001 root@$SECOND_SERVER "rm -f ~/stats-file.log" &
	ssh_no_key -p 20001 root@$SECOND_SERVER "rm -f ~/log-file.log" &
	ssh_no_key root@$SECOND_SERVER "pkill -USR2 dpdk-tagging" &
  ssh_no_key root@$THIRD_SERVER "pkill -USR2 dpdk-tagging" & 
	wait
}

get_stats() {
	filename=$1

	echo "Getting $FIRST_SERVER stats..."
	ssh_no_key -p 20001 root@$FIRST_SERVER "cat ~/stats-file.log" > "$filename-client-stats.log"
	ssh_no_key -p 20001 root@$FIRST_SERVER "cat ~/log-file.log" > "$filename-client-log.log"
	ssh_no_key root@$FIRST_SERVER "pkill -USR1 dpdk-tagging ; docker logs dpdk | grep parsable-stats | cut -d = -f 2 | tail -n 2" > "$filename-client-dpdk.log" 
	echo "Getting $SECOND_SERVER stats..."
	ssh_no_key -p 20001 root@$SECOND_SERVER "cat ~/log-file.log" > "$filename-server-log.log"
	ssh_no_key root@$SECOND_SERVER "pkill -USR1 dpdk-tagging ; docker logs dpdk | grep parsable-stats | cut -d = -f 2 | tail -n 2" > "$filename-server-dpdk.log" 
	scp_no_key -P 20001 root@$SECOND_SERVER:"~/server.pcapng" $filename-server.pcapng

  echo "Getting moongen stats..."
  echo "$THIRD_SERVER" > "$filename-moongen-log.log"
  ssh_no_key -p 20001 root@$THIRD_SERVER "docker logs moongen" >> "$filename-moongen-log.log"
  echo "$FIRST_SERVER" >> "$filename-moongen-log.log"
  ssh_no_key -p 20002 root@$FIRST_SERVER "docker logs moongen" >> "$filename-moongen-log.log"

  echo "Getting $THIRD_SERVER stats..."
  ssh_no_key root@$THIRD_SERVER "pkill -USR1 dpdk-tagging ; docker logs dpdk | grep parsable-stats | cut -d = -f 2 | tail -n 2" > "$filename-moongen-dpdk.log" 
}

configure_interface_on_vms() {
	echo "Configuring VM interfaces and installing PTPd..."
	# Configure DNS server
	ssh_no_key -p 20001 root@$FIRST_SERVER  "sed -i \"s/^nameserver.*/nameserver 8.8.8.8/\" /etc/resolv.conf" > /dev/null &
	ssh_no_key -p 20001 root@$SECOND_SERVER "sed -i \"s/^nameserver.*/nameserver 8.8.8.8/\" /etc/resolv.conf" > /dev/null &
	wait
	# Install PTPd
	ssh_no_key -p 20001 root@$FIRST_SERVER  "apt-get -y update && apt-get -y install ptpd" > /dev/null 2>&1 &
	ssh_no_key -p 20001 root@$SECOND_SERVER "apt-get -y update && apt-get -y install ptpd" > /dev/null 2>&1 &
	wait
	# Configure IP address
	ssh_no_key -p 20001 root@$FIRST_SERVER  "ip link set dev eth1 down" & 
	ssh_no_key -p 20001 root@$SECOND_SERVER "ip link set dev eth1 down" &
	wait
	ssh_no_key -p 20001 root@$FIRST_SERVER  "ip addr add 30.0.0.1/24 dev eth1" & 
	ssh_no_key -p 20001 root@$SECOND_SERVER "ip addr add 30.0.0.2/24 dev eth1" &
	wait
	ssh_no_key -p 20001 root@$FIRST_SERVER  "ip link set dev eth1 up" &
	ssh_no_key -p 20001 root@$SECOND_SERVER "ip link set dev eth1 up" &
	wait
	# Configure ARP entries
	ssh_no_key -p 20001 root@$FIRST_SERVER  "arp -s 30.0.0.2 02:9e:66:a6:d0:01"
	ssh_no_key -p 20001 root@$SECOND_SERVER "arp -s 30.0.0.1 02:ca:4e:00:00:01"
	wait
}

start_flows() {
	packet_size=$1
	duration=$2
	n_flows=1

	echo "Starting tcpdump on server..."
	ssh_no_key -p 20001 root@$SECOND_SERVER "sh -c 'nohup tcpdump -s 90 -i eth1 -w ~/server.pcapng > /dev/null 2>&1 &'"

  echo "Starting MoonGen on third server..."
  ssh_no_key -p 20001 root@$THIRD_SERVER "~/moongen/docker/run_docker.sh l3-load-with-bursts $packet_size $duration $n_flows 500 10" > /dev/null &
  ssh_no_key -p 20002 root@$FIRST_SERVER "~/moongen/docker/run_docker.sh l3-load-with-bursts $packet_size $duration $n_flows 500 10" > /dev/null &

	# Wait for moongen to start
	sleep 8

	echo "Starting PTPd server..."
	ssh_no_key -p 20001 root@$SECOND_SERVER "rm -f ~/log-file.log && rm -f ~/stats-file.log && ptpd -f log-file.log -S stats-file.log -i eth1 -M -U -u 30.0.0.1 -r -5" > /dev/null &
	echo "Starting PTPd client..."
	ssh_no_key -p 20001 root@$FIRST_SERVER "rm -f ~/log-file.log && rm -f ~/stats-file.log && ptpd -f log-file.log -S stats-file.log -i eth1 -s -U -u 30.0.0.2 -r -5" > /dev/null &

	sleep 30
	
	# Stop tcpdump after 10 seconds
	ssh_no_key -p 20001 root@$SECOND_SERVER "pkill tcpdump"

	sleep $(($duration - 30))

	echo "Stopping PTPd..."
	ssh_no_key -p 20001 root@$FIRST_SERVER  "killall ptpd" > /dev/null &
	ssh_no_key -p 20001 root@$SECOND_SERVER "killall ptpd" > /dev/null &

	wait
}


duration=320
packet_size=1500
current_time=$(date "+%Y-%m-%d_%H-%M-%S")
filename_prefix="ptp-logs/ptp-$current_time-$packet_size-$with_moongen"

delete_running_vms
stop_dpdk_tagging
start_dpdk_tagging
sleep 8
create_vms
configure_moongen_on_vms
build_moongen_on_vms
configure_interface_on_vms
insert_tagging_rules
clear_stats
start_flows $packet_size $duration
get_stats $filename_prefix
