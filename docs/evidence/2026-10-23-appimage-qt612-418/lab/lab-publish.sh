#!/bin/sh
# lab.sh ARGS...: lab_publish.py on d69 with the live signer's environment (AWS and GnuPG paths, read by path; its
# configuration, state, checkout and timer are not touched), this pass's own state directory and throwaway lab keys.
# #418: one lab nightly (the PR head's AppImage) for 1058 -> it through the lab feed. pkgrepo.py comes from the PR clone.
set -eu
ROOT=/home/patrick/work/put.gd-qt418/lab
export GNUPGHOME=$HOME/.local/share/putgd-signer/gnupg AWS_CONFIG_FILE=$HOME/.config/putgd-signer/aws-config \
  AWS_SHARED_CREDENTIALS_FILE=$HOME/.config/putgd-signer/aws-credentials AWS_SDK_LOAD_CONFIG=1 PYTHONDONTWRITEBYTECODE=1
exec python3 -I $ROOT/tool/lab_publish.py /home/patrick/work/put.gd-qt418/src $HOME/.config/putgd-signer/signer.json $ROOT/labstate $ROOT/keys "$@"
