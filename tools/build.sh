#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tools/dependencies.sh
if [[ $# == 0 ]]; then set -- build; fi
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/project" -w /project \
  espressif/idf:v5.5.5@sha256:a9231d0697ab8f7517cc072e93b7c83e04907bfbfba80b6440d7dbbf90665cf2 \
  idf.py "$@"
