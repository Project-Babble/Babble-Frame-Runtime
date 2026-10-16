#!/usr/bin/env bash
# Copies this repository to the Steam Frame, and optionally builds it there.
#
#   tools/sync_to_frame.sh [--host USER@HOST] [--build] [--tests] [--profile NAME]
#
#   --host     ssh target (default: $FRAME_HOST, else steamos@frame.local)
#   --build    build the driver folder on the headset (~/babble-frame-runtime/build)
#   --tests    build the test build (build-dev) and run the unit tests
#   --profile  build profile (cmake/babble_profile.cmake): dev (default) or
#              release; a profile other than dev builds in build-<profile> and
#              build-<profile>-tests, so the dev build dirs stay as they are
#
# The source lands in ~/babble-frame-runtime/src on the headset, next to the
# build directories (see docs/building.md). Before copying, the current commit
# is written to .babble-rev, because the copy has no .git and dev builds show
# their commit in the version ("V0.1 Alpha (dev 1e20047)").
#
# It never restarts the runtime or SteamVR: restarting turns the cameras on.
# To run a new build: pkill -TERM -x babble-runtime (the shim restarts it).
set -euo pipefail

host="${FRAME_HOST:-steamos@frame.local}"
build=false
tests=false
profile=dev
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) host="${2:?--host needs a value}"; shift 2 ;;
    --build) build=true; shift ;;
    --tests) tests=true; shift ;;
    --profile) profile="${2:?--profile needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root="babble-frame-runtime"   # This path is on the headset, relative to the home directory.

git -C "${repo}" describe --always --dirty --abbrev=7 > "${repo}/.babble-rev"
echo "commit $(cat "${repo}/.babble-rev") -> ${host}:${root}/src"

rsync -a \
  --exclude .git --exclude .md --exclude /build --exclude '/build-*' \
  --exclude 'third_party/nlohmann_json/tests' --exclude 'third_party/nlohmann_json/docs' \
  --exclude 'third_party/doctest/examples' \
  "${repo}/" "${host}:${root}/src/"

if [[ "${profile}" == dev ]]; then
  bdir=build; tdir=build-dev
else
  bdir="build-${profile}"; tdir="build-${profile}-tests"
fi

if ${build}; then
  echo "building ${root}/${bdir} (${profile} profile)"
  ssh "${host}" "set -e; cd ${root}
    [ -f ${bdir}/CMakeCache.txt ] || cmake -S src -B ${bdir} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBABBLE_TESTS=OFF -DBABBLE_PROFILE=${profile}
    cmake --build ${bdir} -j\$(nproc)"
fi

if ${tests}; then
  echo "building and running the tests in ${root}/${tdir} (${profile} profile)"
  ssh "${host}" "set -e; cd ${root}
    [ -f ${tdir}/CMakeCache.txt ] || cmake -S src -B ${tdir} -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBABBLE_TESTS=ON -DBABBLE_PROFILE=${profile}
    cmake --build ${tdir} -j\$(nproc)
    ${tdir}/runtime/babble_tests"
fi
