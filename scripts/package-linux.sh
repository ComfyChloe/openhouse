#!/usr/bin/env bash
# Packages the Linux driver and tools from a release build:
#   scripts/package-linux.sh [BUILD_DIR]     (default build-release)
# Output: dist/openhouse-driver/ and dist/openhouse-driver-linux.tar.gz
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
build="${1:-$repo/build-release}"
out="$repo/dist/openhouse-driver"
rm -rf "$out"
mkdir -p "$out/tools"
python3 "$repo/scripts/package-driver.py" "$build/openvr/lighthouse/bin/linux64/driver_lighthouse.so" "$out" | tail -n1
cp "$repo/packaging/install-linux.sh" "$out/install.sh"
cp "$repo/packaging/uninstall-linux.sh" "$out/uninstall.sh"
cp "$repo/packaging/steamvr-dir.sh" "$out/.steamvr-dir.sh"
cp "$repo/packaging/settingsschema.vrsettings" "$out/"
for tool in lhtool lhcapture openhouse-viewer openhouse-check; do
  cp "$build/$tool" "$out/tools/"
  strip "$out/tools/$tool"
done
tar czf "$repo/dist/openhouse-driver-linux.tar.gz" -C "$repo/dist" openhouse-driver
echo "Packaged $repo/dist/openhouse-driver-linux.tar.gz"
