#!/bin/sh
# lab.sh ARGS...: lab_publish.py on d69 with the live signer's environment (AWS and GnuPG paths, read by path; its
# configuration, state, checkout and timer are not touched), this pass's own state directory and lab stable keys.
# pkgrepo.py comes from the pass's lab source tree (N+1's commit plus the lab key commit; pkgrepo.py itself unchanged).
set -eu
ROOT=/home/patrick/work/put.gd-ua
export GNUPGHOME=$HOME/.local/share/putgd-signer/gnupg AWS_CONFIG_FILE=$HOME/.config/putgd-signer/aws-config \
  AWS_SHARED_CREDENTIALS_FILE=$HOME/.config/putgd-signer/aws-credentials AWS_SDK_LOAD_CONFIG=1 PYTHONDONTWRITEBYTECODE=1
exec python3 -I $ROOT/tool/lab_publish.py $ROOT/lab/src $HOME/.config/putgd-signer/signer.json $ROOT/labstate $ROOT/keys "$@"
