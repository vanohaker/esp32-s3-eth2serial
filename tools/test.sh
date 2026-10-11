#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/.build
g++ -std=c++20 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -g -Icomponents/knife_core/include \
  components/knife_core/core.cpp tests/core_test.cpp -o tests/.build/core_test
tests/.build/core_test
node --check main/web/app.js
