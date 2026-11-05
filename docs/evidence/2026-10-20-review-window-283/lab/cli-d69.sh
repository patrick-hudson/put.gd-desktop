#!/bin/bash
# The lab host's production putgd-cli with identical arguments (MCP e2e for #283).
exec ssh -o BatchMode=yes -o IdentitiesOnly=yes -o UserKnownHostsFile=$HOME/work/put.gd/work/remote-builders/d69/known_hosts -i $HOME/.ssh/awskey_110724 patrick@192.168.68.69 "$(printf '%q ' /home/patrick/work/put.gd-r283/build/dev/putgd-cli "$@")"
