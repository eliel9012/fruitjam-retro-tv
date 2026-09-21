#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p work
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itests/stubs -Iinclude tests/test_core.cpp src/InputManager.cpp src/NetworkManager.cpp \
  -o work/test_core
./work/test_core "$@"
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itests/portal_stubs -Iinclude tests/test_portal.cpp src/ConfigurationPortal.cpp \
  -o work/test_portal
./work/test_portal
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Itests/stubs -Iinclude tests/test_id3.cpp \
  -o work/test_id3
./work/test_id3
python3 tools/sync_schematik.py --check
