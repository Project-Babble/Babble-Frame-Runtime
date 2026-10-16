#!/usr/bin/env bash
# Removes a one-off install made by install.sh. Run it on the headset (a copy
# sits next to the install):
#
#   ~/.local/share/babble-frame-runtime/uninstall.sh [--dir DIR] [--purge] [--no-restart]
#
#   --dir         the installed driver folder
#                 (default: ~/.local/share/babble-frame-runtime/babble)
#   --purge       also remove settings and logs (~/.config/babble-frame-runtime,
#                 ~/.local/state/babble-frame-runtime)
#   --no-restart  don't restart SteamVR (see below)
#
# The folder is unregistered from SteamVR, SteamVR is restarted so the driver
# unloads, and then the folder is deleted. With --no-restart the folder is
# deleted while the driver is still loaded; its shim notices and cleans up the
# logs itself, as for a Steam uninstall. A Steam install or dev build that
# install.sh unregistered is not re-registered: launch it from Steam, or run
# its bin/linuxarm64/babble-setup.
set -euo pipefail

dir="${HOME}/.local/share/babble-frame-runtime/babble"
restart=true
purge=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir) dir="${2:?--dir needs a value}"; shift 2 ;;
    --purge) purge=true; shift ;;
    --no-restart) restart=false; shift ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done
dir="${dir%/}"

setup="$dir/bin/linuxarm64/babble-setup"
if [[ -x "$setup" ]]; then
  if $purge; then
    "$setup" --uninstall   # This removes the registration, the settings and the logs.
  else
    "$setup" --unregister --no-restart
  fi
elif [[ -e "$dir" ]]; then
  echo "$dir has no babble-setup; is it a Babble driver folder?" >&2
  exit 1
else
  echo "nothing installed in $dir"
  if $purge; then
    rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/babble-frame-runtime" \
           "${XDG_STATE_HOME:-$HOME/.local/state}/babble-frame-runtime"
    echo "removed settings and logs"
  fi
  exit 0
fi

if $restart && pgrep -x babble-runtime >/dev/null && systemctl --user --quiet is-active steamvr.service 2>/dev/null; then
  echo "restarting SteamVR to unload the driver"
  systemctl --user restart steamvr.service
fi

rm -rf "$dir"
# Remove the copy of this script if it is in the default location.
[[ "$(dirname "$dir")" == "$HOME/.local/share/babble-frame-runtime" ]] && rm -f "$(dirname "$dir")/uninstall.sh"
rmdir "$(dirname "$dir")" 2>/dev/null || true
echo "removed $dir"
