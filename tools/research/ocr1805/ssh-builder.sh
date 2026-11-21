#!/bin/bash
# b HOST cmd... : pinned BatchMode ssh to a builder
h=$1; shift
case $h in f44) ip=192.168.68.70;; d69) ip=192.168.68.69;; k72) ip=192.168.68.72;; m74) ip=192.168.68.74;; aurora-amd) ip=192.168.68.71;; *) echo bad host; exit 2;; esac
exec ssh -i /home/patrick/.ssh/awskey_110724 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/$h/known_hosts patrick@$ip "$@"
