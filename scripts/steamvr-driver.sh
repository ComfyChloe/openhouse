#!/usr/bin/env bash
# Install / remove the liblh drop-in driver_lighthouse.so in a SteamVR installation.
# Used by both SteamVR and Monado's steamvr_lh (which loads the same file).
#
#   scripts/steamvr-driver.sh install [path/to/driver_lighthouse.so]
#   scripts/steamvr-driver.sh uninstall      # restores SteamVR's original driver
#   scripts/steamvr-driver.sh status
#
# STEAMVR_DIR overrides the SteamVR location.
set -euo pipefail

find_steamvr() {
  if [[ -n "${STEAMVR_DIR:-}" ]]; then echo "$STEAMVR_DIR"; return; fi
  for d in "$HOME/.local/share/Steam/steamapps/common/SteamVR" "$HOME/.steam/steam/steamapps/common/SteamVR" \
           "$HOME/.steam/root/steamapps/common/SteamVR"; do
    [[ -d "$d/drivers/lighthouse" ]] && { echo "$d"; return; }
  done
  echo "SteamVR not found; set STEAMVR_DIR" >&2; exit 1
}

steamvr=$(find_steamvr)
dir="$steamvr/drivers/lighthouse/bin/linux64"
target="$dir/driver_lighthouse.so"
backup="$dir/driver_lighthouse.so.stock"
here=$(cd "$(dirname "$0")/.." && pwd)

case "${1:-status}" in
  install)
    src="${2:-$here/build/openvr/lighthouse/bin/linux64/driver_lighthouse.so}"
    [[ -f "$src" ]] || { echo "driver not built: $src" >&2; exit 1; }
    if pgrep -x vrserver >/dev/null || pgrep -x monado-service >/dev/null; then
      echo "stop SteamVR / monado-service first" >&2; exit 1
    fi
    if [[ ! -f "$backup" ]]; then
      cp -p "$target" "$backup"
      echo "backed up the stock driver to $backup"
    fi
    # Steam's container cannot see arbitrary host /usr/lib dependencies.
    # Package their full closure beside the module, with origin-relative RPATH.
    stage=$(mktemp -d)
    trap 'rm -rf "$stage"' EXIT
    python3 "$here/scripts/package-driver.py" "$src" "$stage"
    mkdir -p "$dir/lh-libs"
    cp -a "$stage/lh-libs/." "$dir/lh-libs/"
    cp -a "$stage/lh-licenses" "$dir/"
    cp "$stage/lh-dependencies.json" "$dir/"
    cp "$stage/driver_lighthouse.so" "$target.new"
    mv "$target.new" "$target"
    echo "installed $(du -h "$target" | cut -f1) liblh driver to $target"
    ;;
  uninstall)
    if pgrep -x vrserver >/dev/null || pgrep -x monado-service >/dev/null; then
      echo "stop SteamVR / monado-service first" >&2; exit 1
    fi
    [[ -f "$backup" ]] || { echo "no backup at $backup (stock driver still installed?)" >&2; exit 1; }
    mv "$backup" "$target"
    echo "restored the stock driver"
    ;;
  status)
    # Don't use grep -q with pipefail: strings may get SIGPIPE on a large ELF.
    if strings "$target" | grep 'liblh session started' >/dev/null; then
      echo "liblh driver installed; stock backup: $([[ -f "$backup" ]] && echo yes || echo no)"
    else
      echo "stock driver installed; stock backup: $([[ -f "$backup" ]] && echo yes || echo no)"
    fi
    ;;
  *) echo "usage: $0 install [driver.so] | uninstall | status" >&2; exit 2 ;;
esac
