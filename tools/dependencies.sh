#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
fetch() {
  local name="$1" commit="$2"
  if [[ ! -d "third_party/$name/.git" ]]; then
    mkdir -p "third_party/$name"
    git -C "third_party/$name" init -q
    git -C "third_party/$name" remote add origin "https://github.com/wolfSSL/$name.git"
    git -C "third_party/$name" fetch --depth 1 origin "$commit"
    git -C "third_party/$name" checkout -q --detach FETCH_HEAD
  fi
  [[ "$(git -C "third_party/$name" rev-parse HEAD)" == "$commit" ]] || {
    echo "Unexpected $name revision; preserve local changes and inspect third_party/$name" >&2; exit 1;
  }
  [[ -z "$(git -C "third_party/$name" status --porcelain)" ]] || {
    echo "Modified dependency: $name" >&2; exit 1;
  }
}
fetch wolfssl 3c5eead44904df64e6a5a1f4ebdce377d35a849a
fetch wolfssh 8221e4ee48c93da6cea3325a91de2ecc00ae5f56
