#!/bin/bash
# Prepare the ccache directory that flatpak-builder --ccache bind-mounts at
# /run/ccache. Used by build_flatpak.sh --ccache and by dev_loop.sh, which mount
# the same directory, so a bundle build and a dev build reuse each other's
# objects.
#
# Usage: setup_ccache.sh <state-dir>/ccache

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: ${0##*/} <ccache-dir>" >&2
    exit 2
fi
dir=$1

# The in-sandbox ccache reads this file (CCACHE_DIR=/run/ccache), not ~/.ccache.
# The default 5 GiB max_size evicts within a few OrcaSlicer rebuilds.
#
# No header sloppiness (system_headers, include_file_ctime/mtime): it served
# stale objects after header changes and broke the link. pch_defines and
# time_macros are different: ccache refuses to cache any TU built against a
# precompiled header without them, and libslic3r and libslic3r_gui build almost
# every TU that way. The PCH is still hashed, so a changed header still misses,
# and src/ uses no __DATE__/__TIME__.
mkdir -p "$dir/bin"
cat > "$dir/ccache.conf" <<'CCACHE_CONF'
max_size = 25.0G
sloppiness = pch_defines,time_macros
CCACHE_CONF

# flatpak-builder links only cc, c++, gcc and g++ into /run/ccache/bin, but the
# manifest compiles with clang; ccache in masquerade mode execs the next clang
# on PATH. Linking here rather than setting a compiler launcher keeps every
# module's cache key unchanged.
for compiler in clang clang++; do
    ln -sfn /usr/bin/ccache "$dir/bin/$compiler"
done

echo "ccache: $dir (max 25G, clang and clang++ routed through ccache)"
