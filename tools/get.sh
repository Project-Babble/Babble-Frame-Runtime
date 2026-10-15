#!/usr/bin/env bash
# Installs the newest Babble Frame Runtime release on the Steam Frame. The one
# line on the website, run in Konsole in the headset's Desktop Mode:
#
#   curl -fsSL https://raw.githubusercontent.com/Project-Babble/Babble-Frame-Runtime/main/tools/get.sh | bash
#
# Options, after "bash -s --" when piped (curl ... | bash -s -- --no-restart):
#   --version TAG  install that release (e.g. V1.0-Alpha-1) instead of the newest
#   --prerelease   take the newest release even if it's a prerelease
#   --dir DIR      where the driver folder goes (install.sh's default otherwise)
#   --no-restart   don't restart SteamVR; the new build loads on its next start
#
# Picks the newest stable release (the newest prerelease while there is no
# stable one yet), downloads its tarball, checks it against the release's
# .sha256, and runs the install.sh inside it: no sudo, nothing outside your
# home folder. Updates after that come from the Babble tab in the SteamVR
# dashboard. BABBLE_REPO and BABBLE_API_URL override where releases come from.
set -euo pipefail

# Everything runs from main(), called on the last line, so a download cut short
# never runs half a script.
main() {
  local repo="${BABBLE_REPO:-Project-Babble/Babble-Frame-Runtime}"
  local api="${BABBLE_API_URL:-https://api.github.com/repos/${repo}/releases?per_page=30}"
  local want="" prerelease=0
  local install_args=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --version) want="${2:?--version needs a tag}"; shift 2 ;;
      --prerelease) prerelease=1; shift ;;
      --dir) install_args+=(--dir "${2:?--dir needs a value}"); shift 2 ;;
      --no-restart) install_args+=(--no-restart); shift ;;
      -h|--help) sed -n '2,19p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//' || true; return 0 ;;
      *) echo "unknown option: $1" >&2; return 2 ;;
    esac
  done

  [[ "$(uname -m)" == aarch64 ]] || { echo "This installs on the Steam Frame; run it in the headset's Desktop Mode." >&2; return 1; }
  local tool
  for tool in curl python3 tar xz sha256sum; do
    command -v "$tool" >/dev/null || { echo "$tool is missing; this needs SteamOS on the Steam Frame." >&2; return 1; }
  done

  # Global, not local: the EXIT trap runs after main has returned.
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/babble-get.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT

  echo "Finding the newest Babble Frame Runtime release..."
  curl -fsSL -A babble-get "$api" -o "$tmp/releases.json" ||
    { echo "Couldn't reach GitHub ($api). Check the headset's network and try again." >&2; return 1; }

  # The release to install, as "tag<TAB>tarball url<TAB>sha256 url<TAB>tarball name".
  local pick
  pick="$(python3 -I - "$tmp/releases.json" "$want" "$prerelease" <<'PY'
import json, re, sys

releases = json.load(open(sys.argv[1]))
want, prerelease = sys.argv[2], sys.argv[3] == "1"
stages = {"Alpha": 0, "Beta": 1, "RC": 2, "Release": 3}
found = []
for r in releases if isinstance(releases, list) else []:
    tag = r.get("tag_name", "")
    m = re.fullmatch(r"V(\d+)\.(\d+)[- ](Alpha|Beta|RC|Release)(?:[- ](\d+))?", tag)
    if r.get("draft") or not m:
        continue
    assets = {a.get("name"): a.get("browser_download_url") for a in r.get("assets", [])}
    name = f"babble-frame-runtime.linuxarm64.{tag}.tar.xz"
    if name not in assets or name + ".sha256" not in assets:
        continue
    key = (int(m[1]), int(m[2]), stages[m[3]], int(m[4] or 1))
    found.append((key, bool(r.get("prerelease")), tag, assets[name], assets[name + ".sha256"], name))
if want:
    found = [f for f in found if f[2] == want]
elif not prerelease and any(not f[1] for f in found):
    found = [f for f in found if not f[1]]
if found:
    print("\t".join(max(found)[2:]))
PY
)"
  if [[ -z "$pick" ]]; then
    if [[ -n "$want" ]]; then echo "There's no release $want with a Steam Frame build." >&2
    else echo "There's no Babble Frame Runtime release with a Steam Frame build yet." >&2; fi
    return 1
  fi
  local tag url sha_url name
  IFS=$'\t' read -r tag url sha_url name <<<"$pick"

  echo "Downloading $tag..."
  curl -fL --progress-bar -A babble-get "$url" -o "$tmp/$name" || { echo "The download failed; try again." >&2; return 1; }
  curl -fsSL -A babble-get "$sha_url" -o "$tmp/$name.sha256" || { echo "Couldn't download the checksum; try again." >&2; return 1; }
  local expected actual
  expected="$(awk -v n="$name" '$2 == n || $2 == "*" n { print tolower($1); exit }' "$tmp/$name.sha256")"
  actual="$(sha256sum "$tmp/$name" | awk '{ print $1 }')"
  if [[ -z "$expected" || "$expected" != "$actual" ]]; then
    echo "The download doesn't match the release's checksum; nothing was installed." >&2
    return 1
  fi
  echo "Checksum OK."

  tar -xJf "$tmp/$name" -C "$tmp" install.sh
  bash "$tmp/install.sh" "$tmp/$name" "${install_args[@]}"
  echo "Babble Frame Runtime $tag is installed."
  echo "Its settings are in SteamVR Settings, under Expression Tracking. Turn on Show App Window there"
  echo "to add the Babble tab to the SteamVR dashboard."
}

main "$@"
