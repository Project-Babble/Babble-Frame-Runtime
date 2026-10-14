#!/usr/bin/env bash
# Installs a release build on the Steam Frame, for one-off builds that don't
# come through Steam. Run it on the headset, either from the unpacked release
# tarball (it ships next to babble/) or with the tarball itself:
#
#   tar -xf babble-frame-runtime.linuxarm64.<version>.tar.xz && ./install.sh
#   tools/install.sh <tarball> [--dir DIR] [--no-restart]
#
#   --dir         where the driver folder goes
#                 (default: ~/.local/share/babble-frame-runtime/babble)
#   --no-restart  don't restart SteamVR; the new build loads on its next start
#
# Installing again replaces the previous one-off install; settings and logs
# (~/.config/babble-frame-runtime, ~/.local/state/babble-frame-runtime) are
# kept. The folder is registered with SteamVR by the build's own babble-setup,
# which also drops any other Babble driver registration (a Steam install or a
# dev build), so only this one loads. uninstall.sh is copied next to the
# install (~/.local/share/babble-frame-runtime/uninstall.sh) to undo it.
# The install is marked (babble/.install-source) so the runtime offers its own
# updates from GitHub releases; tools/get.sh downloads a release and runs this.
set -euo pipefail

dir="${HOME}/.local/share/babble-frame-runtime/babble"
restart=true
tarball=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir) dir="${2:?--dir needs a value}"; shift 2 ;;
    --no-restart) restart=false; shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    *) [[ -z "$tarball" ]] || { echo "only one tarball, please" >&2; exit 2; }; tarball="$1"; shift ;;
  esac
done
here="$(cd "$(dirname "$0")" && pwd)"
if [[ -z "$tarball" && ! -e "$here/babble/driver.vrdrivermanifest" ]]; then
  echo "usage: $0 <tarball> [--dir DIR] [--no-restart]" >&2
  exit 2
fi
[[ -z "$tarball" || -f "$tarball" ]] || { echo "no such file: $tarball" >&2; exit 1; }
[[ "$(uname -m)" == aarch64 ]] || { echo "this is an arm64 build for the Steam Frame; run it on the headset" >&2; exit 1; }

dir="${dir%/}"
parent="$(dirname "$dir")"
mkdir -p "$parent"

# Unpack next to the target, so the swap below is a rename on one filesystem.
tmp="$(mktemp -d "${parent}/.babble-install.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
if [[ -n "$tarball" ]]; then
  echo "unpacking $(basename "$tarball")"
  tar -xf "$tarball" -C "$tmp" babble
else
  echo "copying $here/babble"
  cp -a "$here/babble" "$tmp/babble"
fi
new="$tmp/babble"
for f in driver.vrdrivermanifest bin/linuxarm64/driver_babble.so bin/linuxarm64/babble-runtime \
         bin/linuxarm64/babble-setup; do
  [[ -e "$new/$f" ]] || { echo "not a Babble driver tarball: babble/$f is missing" >&2; exit 1; }
done
# Marks a one-off install: the runtime updates itself from GitHub releases only
# when this file is in its folder (never a Steam install or a dev build).
echo "install.sh" > "$new/.install-source"

# Swap the folders back to back: a running driver shim treats a folder that
# stays missing for a couple of seconds as an uninstall and cleans up.
if [[ -e "$dir" ]]; then
  mv "$dir" "$tmp/old"
  mv "$new" "$dir"
  echo "replaced the install in $dir"
else
  mv "$new" "$dir"
  echo "installed to $dir"
fi

"$dir/bin/linuxarm64/babble-setup" --no-restart

# Keep an uninstaller with the install, so the unpacked tarball can go.
uninstall="$parent/uninstall.sh"
if [[ -f "$here/uninstall.sh" ]]; then
  cp "$here/uninstall.sh" "$uninstall"
elif [[ -n "$tarball" ]] && tar -xf "$tarball" -C "$tmp" uninstall.sh 2>/dev/null; then
  cp "$tmp/uninstall.sh" "$uninstall"
fi
[[ -f "$uninstall" ]] && chmod +x "$uninstall"

version="$(grep -o 'Babble Frame Runtime V[^"]*' "$dir/resources/settings/settingsschema.vrsettings" 2>/dev/null | head -1 || true)"
[[ -n "$version" ]] && echo "version: ${version#Babble Frame Runtime }"

if systemctl --user --quiet is-active steamvr.service 2>/dev/null; then
  if $restart; then
    echo "restarting SteamVR to load the new build"
    systemctl --user restart steamvr.service
  else
    echo "SteamVR is running the previous build until it restarts"
  fi
fi
if [[ -f "$uninstall" ]]; then
  [[ "$dir" == "${HOME}/.local/share/babble-frame-runtime/babble" ]] && extra="" || extra=" --dir $dir"
  echo "to uninstall: $uninstall$extra"
fi
