#!/bin/bash
# cov415.sh NAME TAG : cover with pattern on/off, still cover on/off (as dk.sh before F), then seq415.
set -u
n=$1 tag=$2
root=$HOME/work/put.gd-i415; cd "$root" || exit 2; export AFA_ROOT=$root
A="python3 -I tool/afa.py"
$A cover $n start --pattern > runs/$n/trials/$n-$tag-cover1.json 2>&1; echo cover1 $?; sleep 6
$A cover $n stop > /dev/null 2>&1; echo stop1 $?
$A cover $n start > /dev/null 2>&1; echo cover2 $?; sleep 6
$A cover $n stop > /dev/null 2>&1; echo stop2 $?
bash tool/seq415.sh $n $tag
