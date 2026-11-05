#!/bin/bash
# cli.sh METHOD [JSON]: 968's putgd-cli (a client only) against the running resident.
cd ~/i412 && . ./genv.sh
LD_LIBRARY_PATH=~/i412/cli968/squashfs-root/usr/lib:~/i412/cli968/squashfs-root/usr/lib/putgd exec ~/i412/cli968/squashfs-root/usr/bin/putgd-cli "$@"
