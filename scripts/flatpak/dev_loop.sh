#!/bin/bash
# Edit, compile and run OrcaSlicer inside the Flatpak SDK without building a
# bundle: an incremental build against the same /app deps as the bundle, staged
# under a prefix in the Deck's home and run inside the installed app's sandbox.
#
# Run `dev_loop.sh -h` for the commands.

set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cache_dir=${ORCA_DEV_CACHE_DIR:-$repo/.flatpak-builder}
dev_dir=${ORCA_DEV_DIR:-$repo/build_flatpak_dev}
prefix=${ORCA_DEV_PREFIX:-/home/deck/.cache/opencode-scratch/orca-dev}
host=${ORCA_DEV_HOST:-deck@steamdeck.rat-danio.ts.net}
rsh=${ORCA_DEV_SSH:-ssh}
jobs=$(nproc)
with_debuginfo=false
check_after_deploy=true

app_id=com.orcaslicer.OrcaSlicer
src_mnt=/run/build/OrcaSlicer
dev_mnt=/run/build/orca-dev

usage() {
    cat <<EOF
Usage: ${0##*/} <command> [options]

Commands, in the order a first run needs them:
  setup        check out the cached deps into $dev_dir/appdir with
               flatpak-builder --stop-at=OrcaSlicer, then configure the build
               tree. Takes seconds after a ./build_flatpak.sh run in this
               checkout with the same cache dir; otherwise it builds every
               module first. Rerun after deps/ or the manifest's modules change.
  build        incremental ninja build, then stage the install under
               $dev_dir/stage$prefix
  deploy       rsync the staged prefix to HOST:PREFIX, then run check
  check        ldd the deployed binary inside the installed app's sandbox on
               HOST and fail on any unresolved library
  run-cmd      print the command that starts the deployed build on HOST
  shell        interactive shell in the build sandbox, cwd = the build tree

Options:
  -j N               parallel jobs [default: $jobs]
  --cache-dir DIR    flatpak-builder state dir, shared with build_flatpak.sh
                     for the module cache and ccache [default: $cache_dir]
  --dev-dir DIR      app dir, build tree and stage [default: $dev_dir]
  --prefix PATH      where the build runs from on HOST; compiled into the
                     binary as its resources dir [default: $prefix]
  --host USER@HOST   deploy target [default: $host]
  --ssh CMD          ssh command for deploy/check, e.g. 'ssh -p 22022'
                     [default: $rsh]
  --with-debuginfo   setup: match a build_flatpak.sh --with-debuginfo cache
  --no-check         deploy: skip the ldd check

Environment: ORCA_DEV_CACHE_DIR, ORCA_DEV_DIR, ORCA_DEV_PREFIX, ORCA_DEV_HOST
and ORCA_DEV_SSH set the same defaults.

Example, from a checkout with a finished ./build_flatpak.sh --ccache:
  ${0##*/} setup && ${0##*/} build && ${0##*/} deploy
  # edit src/...
  ${0##*/} build && ${0##*/} deploy
EOF
}

die() { echo "${0##*/}: $*" >&2; exit 1; }
step() { echo "==> $*" >&2; }

[ "$#" -gt 0 ] || { usage >&2; exit 2; }
cmd=$1; shift
case $cmd in -h|--help|help) usage; exit 0 ;; esac
while [ "$#" -gt 0 ]; do
    case $1 in
        -j) jobs=$2; shift 2 ;;
        --cache-dir) cache_dir=$2; shift 2 ;;
        --dev-dir) dev_dir=$2; shift 2 ;;
        --prefix) prefix=$2; shift 2 ;;
        --host) host=$2; shift 2 ;;
        --ssh) rsh=$2; shift 2 ;;
        --with-debuginfo) with_debuginfo=true; shift ;;
        --no-check) check_after_deploy=false; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option $1 (see -h)" ;;
    esac
done

cache_dir=$(mkdir -p "$cache_dir" && cd "$cache_dir" && pwd)
dev_dir=$(mkdir -p "$dev_dir" && cd "$dev_dir" && pwd)
appdir=$dev_dir/appdir
obj=$dev_dir/obj
stage=$dev_dir/stage
env_file=$dev_dir/build.env
staged=$stage$prefix

# The build tree is mounted where flatpak-builder builds the OrcaSlicer module
# (/run/build/OrcaSlicer/build_flatpak), and the compilers and flags are the
# module's, so the compile commands match a bundle build and the shared ccache
# serves both.
sandbox() {
    local -a args env_args
    mapfile -t env_args < "$env_file"
    mkdir -p "$obj" "$stage" "$repo/build_flatpak"
    args=(--die-with-parent --nofilesystem=host:reset
          --filesystem="$repo" --bind-mount="$src_mnt=$repo"
          --filesystem="$obj" --bind-mount="$src_mnt/build_flatpak=$obj"
          --filesystem="$dev_dir" --bind-mount="$dev_mnt=$dev_dir"
          --bind-mount=/run/ccache="$cache_dir/ccache")
    # A linked worktree's .git points into the main checkout; without it the
    # build cannot read the commit hash and the About box says 0000000.
    local common
    common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
    if [ -n "$common" ] && [[ $common != "$repo"/* ]]; then
        args+=(--filesystem="$common:ro")
    fi
    flatpak build "${args[@]}" "${env_args[@]}" --env=FLATPAK_BUILDER_N_JOBS="$jobs" \
        "$@"
}

in_src() { sandbox --build-dir="$src_mnt" "$appdir" "$@"; }

need_setup() {
    [ -f "$appdir/metadata" ] && [ -s "$env_file" ] || die "run '${0##*/} setup' first"
}

configure() {
    local manifest=$repo/scripts/flatpak/com.orcaslicer.OrcaSlicer.yml cmake_cmd
    # Reuse the manifest's configure command so flags never drift from the
    # bundle; only the install prefix and the generator differ.
    cmake_cmd=$(awk '
        /^  - name: OrcaSlicer[[:space:]]*$/ { m = 1; next }
        m && /^      - \|[[:space:]]*$/ { c = 1; next }
        c && /^      - / { exit }
        c { sub(/^[[:space:]]+/, ""); print }' "$manifest")
    [[ $cmake_cmd == "cmake . -B build_flatpak"* ]] ||
        die "cannot find the OrcaSlicer configure command in $manifest"
    grep -q -- '-DCMAKE_INSTALL_PREFIX=/app' <<<"$cmake_cmd" ||
        die "OrcaSlicer configure command no longer sets -DCMAKE_INSTALL_PREFIX=/app"
    cmake_cmd=${cmake_cmd/-DCMAKE_INSTALL_PREFIX=\/app/-DCMAKE_INSTALL_PREFIX=$prefix}
    cmake_cmd=${cmake_cmd/cmake . -B build_flatpak/cmake . -B build_flatpak -G Ninja}
    step "configuring (prefix $prefix)"
    in_src sh -c "$cmake_cmd"
}

# flatpak-builder keys its module cache on the manifest's absolute path, so this
# must be the path build_flatpak.sh builds from for setup to reuse its modules.
dev_manifest=$repo/scripts/flatpak/com.orcaslicer.OrcaSlicer.generated.yml

cmd_setup() {
    local -a gen_args=() common_args
    [ ! -e "$dev_manifest" ] ||
        die "$dev_manifest exists: a build_flatpak.sh run is in progress here, or one was killed (then delete it)"
    trap 'rm -f "$dev_manifest"' EXIT
    [ "$with_debuginfo" = true ] && gen_args+=(--with-debuginfo)

    (cd "$repo" && ./scripts/flatpak/make_deps_tar.sh)
    "$repo/scripts/flatpak/generate_manifest.sh" "${gen_args[@]}" "$dev_manifest"
    "$repo/scripts/flatpak/setup_ccache.sh" "$cache_dir/ccache"

    common_args=(--arch="$(uname -m)" --state-dir="$cache_dir" --ccache)
    step "checking out deps into $appdir (a cache miss here builds them)"
    FLATPAK_BUILDER_N_JOBS=$jobs flatpak-builder "${common_args[@]}" \
        --user --install-deps-from=flathub --delete-build-dirs --disable-rofiles-fuse --jobs="$jobs" \
        --force-clean --stop-at=OrcaSlicer "$appdir" "$dev_manifest"

    # flatpak-builder --run applies the manifest's build-options env the way a
    # module build does, but leaves out the SDK's default compiler flags, which
    # a module build uses for any flag variable the manifest does not set.
    step "recording the module build environment"
    local run_env sdk_ref sdk_defaults
    run_env=$(flatpak-builder "${common_args[@]}" --run "$appdir" "$dev_manifest" env)
    sdk_ref=$(sed -n 's/^sdk=//p' "$appdir/metadata")
    sdk_defaults=$(flatpak info --show-location "$sdk_ref")/files/etc/flatpak-builder/defaults.json
    [ -f "$sdk_defaults" ] || die "no SDK build defaults at $sdk_defaults"
    python3 -c '
import json, sys
defaults = json.load(open(sys.argv[1]))
env = dict(line.split("=", 1) for line in sys.stdin.read().splitlines() if "=" in line)
for var in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS"):
    if var not in env and defaults.get(var.lower()):
        env[var] = defaults[var.lower()]
for var in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "PATH", "LD_LIBRARY_PATH",
            "PKG_CONFIG_PATH", "ACLOCAL_PATH", "CCACHE_DIR"):
    if var in env:
        print(f"--env={var}={env[var]}")
' "$sdk_defaults" <<<"$run_env" > "$env_file.tmp"
    grep -q '^--env=PATH=/run/ccache/bin:' "$env_file.tmp" ||
        die "build environment has no /run/ccache/bin on PATH: $(cat "$env_file.tmp")"
    mv "$env_file.tmp" "$env_file"

    rm -rf "$obj"
    configure
}

cmd_build() {
    need_setup
    [ -f "$obj/CMakeCache.txt" ] || configure
    grep -q "^CMAKE_INSTALL_PREFIX:PATH=$prefix\$" "$obj/CMakeCache.txt" || configure

    step "building"
    in_src cmake --build build_flatpak -j"$jobs"

    local stamp=$dev_dir/.gettext-stamp
    if [ ! -e "$stamp" ] || [ -n "$(find "$repo/localization/i18n" -name '*.po' -newer "$stamp" -print -quit)" ]; then
        step "compiling translations"
        in_src ./scripts/run_gettext.sh >/dev/null
        touch "$stamp"
    fi

    step "staging into $staged"
    in_src env DESTDIR="$dev_mnt/stage" cmake --install build_flatpak >/dev/null
    # The prefix is a host path: the in-sandbox install wrote it under
    # $dev_mnt/stage, which is $stage on the host.
    in_src sh -euc '
        p=$1; s=$2
        strip --strip-debug "$s$p/bin/orca-slicer"
        # Ship every /app library the binary resolves against, so it runs with
        # exactly the deps it was linked to, whatever bundle the Deck has
        # installed. libpython comes as a tree: the rpath and the Python home
        # both look for it next to bin/.
        rm -rf "$s$p/lib"; mkdir -p "$s$p/lib"
        ldd "$s$p/bin/orca-slicer" | sed -n "s#.* => \(/app/[^ ]*\) .*#\1#p" | grep -v "^/app/libpython/" |
            while read -r lib; do cp -pL "$lib" "$s$p/lib/"; done
        mkdir -p "$s$p/libpython"
        cp -a /app/libpython/. "$s$p/libpython/"
        ln -sfn ../libpython "$s$p/bin/python"
    ' sh "$prefix" "$dev_mnt/stage"

    grep -q '^exec /app/bin/orca-slicer "\$@"$' "$repo/scripts/flatpak/entrypoint" ||
        die "scripts/flatpak/entrypoint no longer ends in exec /app/bin/orca-slicer"
    sed "s#^exec /app/bin/orca-slicer \"\$@\"\$#export LD_LIBRARY_PATH=$prefix/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\nexec $prefix/bin/orca-slicer \"\$@\"#" \
        "$repo/scripts/flatpak/entrypoint" > "$staged/bin/entrypoint"
    chmod 755 "$staged/bin/entrypoint"
    cat > "$staged/orca-dev.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=OrcaSlicer (dev build)
Exec=flatpak run --command=$prefix/bin/entrypoint $app_id %U
Icon=$prefix/share/OrcaSlicer/images/OrcaSlicer_192px.png
Categories=Graphics;3DGraphics;Engineering;
EOF
    step "staged $(du -sh "$staged" | cut -f1) ($(du -h "$staged/bin/orca-slicer" | cut -f1) binary)"
}

cmd_check() {
    step "ldd inside $app_id on $host"
    local out
    # shellcheck disable=SC2029
    out=$($rsh "$host" "flatpak run --command=sh $app_id -c 'LD_LIBRARY_PATH=$prefix/lib ldd $prefix/bin/orca-slicer'") ||
        die "ldd failed on $host"
    if grep -q 'not found' <<<"$out"; then
        grep 'not found' <<<"$out" >&2
        die "unresolved libraries on $host"
    fi
    echo "all $(grep -c '=>' <<<"$out") libraries resolve; $(grep -c "=> $prefix/" <<<"$out") from the dev prefix"
}

cmd_deploy() {
    [ -x "$staged/bin/orca-slicer" ] || die "nothing staged; run '${0##*/} build' first"
    step "rsync $staged/ -> $host:$prefix/"
    # shellcheck disable=SC2029
    $rsh "$host" "mkdir -p '$prefix'"
    rsync -az --delete --info=stats1 -e "$rsh" "$staged/" "$host:$prefix/"
    [ "$check_after_deploy" = false ] || cmd_check
}

cmd_run_cmd() {
    cat <<EOF
# On the Deck (Konsole, or a non-Steam game entry):
flatpak run --command=$prefix/bin/entrypoint $app_id
# Desktop Mode launcher: cp $prefix/orca-dev.desktop ~/.local/share/applications/
# The dev build shares the installed app's config dir; pass --datadir DIR to
# keep it apart while the installed OrcaSlicer is open.
EOF
}

cmd_shell() {
    need_setup
    sandbox --build-dir="$src_mnt/build_flatpak" "$appdir" bash
}

case $cmd in
    setup) cmd_setup ;;
    build) cmd_build ;;
    deploy) cmd_deploy ;;
    check) cmd_check ;;
    run-cmd) cmd_run_cmd ;;
    shell) cmd_shell ;;
    *) die "unknown command $cmd (see -h)" ;;
esac
