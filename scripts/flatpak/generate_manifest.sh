#!/bin/bash
# Write the manifest build_flatpak.sh and dev_loop.sh build from: the checked-in
# com.orcaslicer.OrcaSlicer.yml plus the commit hash and the debuginfo choice.
# Both scripts must generate it the same way, or dev_loop.sh's --stop-at build
# misses the module cache a bundle build left behind.
#
# Usage: generate_manifest.sh [--hash HASH] [--with-debuginfo] OUT

set -euo pipefail

hash=""
debuginfo=false
out=""
while [ "$#" -gt 0 ]; do
    case $1 in
        --hash) hash=$2; shift 2 ;;
        --with-debuginfo) debuginfo=true; shift ;;
        -*) echo "${0##*/}: unknown option $1" >&2; exit 2 ;;
        *) out=$1; shift ;;
    esac
done
[ -n "$out" ] || { echo "usage: ${0##*/} [--hash HASH] [--with-debuginfo] OUT" >&2; exit 2; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cp "$script_dir/com.orcaslicer.OrcaSlicer.yml" "$out"

# The manifest copies the tree without .git, so CMake cannot read the hash in the
# sandbox and would embed "0000000". Prefixing it onto the OrcaSlicer module's
# configure command, rather than the global build-options, keeps the wxWidgets
# and deps module caches valid across commits.
if [ -n "$hash" ]; then
    sed -i "s#cmake \. -B build_flatpak#git_commit_hash=\"$hash\" cmake . -B build_flatpak#" "$out"
    echo "manifest: embedding commit hash $hash"
fi

# Global build-options are part of every module's cache key, so flipping this
# rebuilds everything.
if [ "$debuginfo" = false ]; then
    sed -i '/^build-options:/a\  no-debuginfo: true\n  strip: true' "$out"
    echo "manifest: debug info disabled"
fi
