#!/bin/bash
# motion CLIP | fetch-motion URL: putgd-record --motion on the lab host.
R=/home/patrick/work/put.gd-r283/build/dev/putgd-record
ssh_() { ssh -o BatchMode=yes -o IdentitiesOnly=yes -o UserKnownHostsFile=$HOME/work/put.gd/work/remote-builders/d69/known_hosts -i $HOME/.ssh/awskey_110724 patrick@192.168.68.69 "$@"; }
case "$1" in
  motion) ssh_ "$(printf '%q ' $R --motion "$2")" ;;
  fetch-motion) ssh_ "f=/home/patrick/putgd-lab/r283/e2e/out/uploaded.mp4; curl -sf $(printf '%q' "$2") -o \$f && sha256sum \$f | cut -c1-16 && $R --motion \$f" ;;
esac
