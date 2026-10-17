#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# One command to run startracker-cpp on a Raspberry Pi camera:
# installs packages, builds, gets the right star database, and starts tracking.
#
#   scripts/raspi/run.sh --camera hq-16mm              # HQ Camera + 16 mm lens
#   scripts/raspi/run.sh --camera gs-12mm --exposure-ms 200 --gain 16
#   scripts/raspi/run.sh --camera cm3 --frames 50 --csv attitude.csv
#   scripts/raspi/run.sh --camera hq-16mm --bench      # measure speed on this Pi instead
#   scripts/raspi/run.sh --fov 11.4 --from-pgm frames/ # no camera: track saved PGM frames
#
# Every step is skipped when already done, so re-running is fast.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$REPO/build/pi}"

usage() {
  cat <<'USAGE'
Usage: scripts/raspi/run.sh [--camera NAME] [options] [-- extra pi_tracker.py options]

Cameras (sets the raw mode, field of view and database):
  hq-12mm hq-16mm hq-25mm   Raspberry Pi HQ Camera (IMX477) with a 12/16/25 mm C-mount lens
  gs-12mm gs-16mm           Raspberry Pi Global Shutter Camera (IMX296) with a 12/16 mm lens
  cm3                       Camera Module 3 (IMX708), built-in lens, ~66 deg
  cm2                       Camera Module 2 (IMX219), built-in lens, ~62 deg

Options:
  --fov DEG          horizontal field of view (overrides the camera preset; use for other lenses)
  --mode WxH         sensor raw mode (overrides the camera preset)
  --db FILE          star database (.stdb) to use instead of the automatic choice
  --exposure-ms MS   exposure time (default 100)
  --gain G           analogue gain (default 8)
  --frames N         stop after N frames
  --csv FILE         log one line per frame
  --save-pgm DIR     save frames for later replay
  --from-pgm DIR     track saved PGM frames instead of a camera (needs --fov)
  --bench            run st_bench at this camera's resolution and exit
  --setup-only       install, build and fetch the database, then exit
  --no-apt           do not run apt-get (install the packages yourself)
  -h, --help         this help
USAGE
}

CAMERA="" FOV="" MODE="" DB="" BENCH=0 SETUP_ONLY=0 NO_APT=0 FROM_PGM=""
PASS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --camera) CAMERA="$2"; shift 2 ;;
    --fov) FOV="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --db) DB="$2"; shift 2 ;;
    --from-pgm) FROM_PGM="$2"; shift 2 ;;
    --exposure-ms|--gain|--frames|--csv|--save-pgm|--period-ms) PASS+=("$1" "$2"); shift 2 ;;
    --no-roi) PASS+=("$1"); shift ;;
    --bench) BENCH=1; shift ;;
    --setup-only) SETUP_ONLY=1; shift ;;
    --no-apt) NO_APT=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; PASS+=("$@"); break ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# Camera presets: raw mode (2x2-summed to half size by pi_tracker.py) and horizontal FOV.
P_MODE="" P_FOV=""
case "$CAMERA" in
  hq-12mm) P_MODE=2028x1520 P_FOV=29.4 ;;
  hq-16mm) P_MODE=2028x1520 P_FOV=22.2 ;;
  hq-25mm) P_MODE=2028x1520 P_FOV=14.3 ;;
  gs-12mm) P_MODE=1456x1088 P_FOV=23.6 ;;
  gs-16mm) P_MODE=1456x1088 P_FOV=17.8 ;;
  cm3)     P_MODE=2304x1296 P_FOV=66 ;;
  cm2)     P_MODE=1640x1232 P_FOV=62.2 ;;
  "") ;;
  *) echo "unknown camera '$CAMERA' (see --help)" >&2; exit 2 ;;
esac
FOV="${FOV:-$P_FOV}"
MODE="${MODE:-$P_MODE}"
if [[ -z "$FOV" && $SETUP_ONLY -eq 0 ]]; then
  echo "pick a camera with --camera (or give --fov)" >&2; usage >&2; exit 2
fi

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

# 1. Packages.
if [[ "$(uname -m)" != "aarch64" && "$(uname -m)" != "x86_64" ]]; then
  echo "warning: $(uname -m) is untested; use 64-bit Raspberry Pi OS" >&2
fi
if [[ $NO_APT -eq 0 ]]; then
  NEED=()
  for c in cmake ninja g++ git; do command -v "$c" >/dev/null || NEED+=("$c"); done
  python3 -c "import numpy" 2>/dev/null || NEED+=(python3-numpy)
  if [[ -z "$FROM_PGM" && $BENCH -eq 0 && $SETUP_ONLY -eq 0 ]]; then
    python3 -c "import picamera2" 2>/dev/null || NEED+=(python3-picamera2)
  fi
  if [[ ${#NEED[@]} -gt 0 ]]; then
    PKGS=()
    for n in "${NEED[@]}"; do
      case "$n" in ninja) PKGS+=(ninja-build) ;; *) PKGS+=("$n") ;; esac
    done
    say "Installing ${PKGS[*]}"
    sudo apt-get update
    sudo apt-get install -y "${PKGS[@]}"
  fi
fi

# 2. Build (release, no tests). Fewer parallel jobs on boards with little memory.
MEM_MB=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 4096)
JOBS=$(nproc 2>/dev/null || echo 2)
if [[ $MEM_MB -lt 1500 ]]; then JOBS=1; elif [[ $MEM_MB -lt 3000 && $JOBS -gt 2 ]]; then JOBS=2; fi
if [[ ! -f "$BUILD/build.ninja" ]]; then
  say "Configuring the build in $BUILD"
  cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DST_BUILD_TESTS=OFF -DST_WERROR=OFF -DST_ASSERTS=OFF
fi
say "Building (jobs: $JOBS)"
cmake --build "$BUILD" -j "$JOBS"

# 3. Star database: the 38 MB cedar database for 10-30 deg, a generated BSC5 database for wide lenses.
if [[ -z "$DB" ]]; then
  if [[ -n "$FOV" ]] && awk "BEGIN {exit !($FOV > 30)}"; then
    DB="$REPO/fixtures/wide_50_75.stdb"
    if [[ ! -f "$DB" ]]; then
      say "Generating a wide-field database (50-75 deg) from the Bright Star Catalogue"
      python3 "$REPO/tools/fetch_fixtures.py" --only bsc5
      "$BUILD/tools/st_dbtool" generate --catalog "$REPO/fixtures/bsc5" --min-fov 50 --max-fov 75 \
        --mag 6 --epoch "$(date +%Y)" -o "$DB"
    fi
  else
    DB="$REPO/fixtures/cedar_default.stdb"
    if [[ ! -f "$DB" ]]; then
      say "Fetching the star database (10-30 deg)"
      python3 "$REPO/tools/fetch_fixtures.py" --only cedar_db --dbtool "$BUILD/tools/st_dbtool"
    fi
  fi
fi
echo "database: $DB"
if [[ -n "$FOV" ]] && { awk "BEGIN {exit !($FOV < 10 || $FOV > 75)}"; }; then
  echo "warning: FOV $FOV deg is outside the automatic databases; generate one with st_dbtool (see docs/raspberry-pi.md)" >&2
fi
[[ $SETUP_ONLY -eq 1 ]] && { say "Setup done"; exit 0; }

# 4. Run.
export STARTRACKER_LIB="$BUILD/src/libstartracker.so"
if [[ $BENCH -eq 1 ]]; then
  W=${MODE%x*} H=${MODE#*x}
  SIZE="$((W / 2))x$((H / 2))"
  say "Benchmark at $SIZE, FOV $FOV deg"
  exec "$BUILD/tools/st_bench" --db "$DB" --size "$SIZE" --fov "$FOV" --frames 40
fi
ARGS=(--db "$DB" --fov "$FOV")
[[ -n "$MODE" ]] && ARGS+=(--mode "$MODE")
[[ -n "$FROM_PGM" ]] && ARGS+=(--from-pgm "$FROM_PGM")
say "Tracking"
exec python3 "$REPO/scripts/raspi/pi_tracker.py" "${ARGS[@]}" ${PASS[@]+"${PASS[@]}"}
