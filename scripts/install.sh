#!/bin/sh
# ReStreamAir — build from source and start the server.
#
#   curl -fsSL https://raw.githubusercontent.com/testingbetaversion/ReStreamAir/main/scripts/install.sh \
#     | sh -s -- --port 1234 --dir /root
#
# Options:
#   --port N      listening port                  (default 8787)
#   --dir PATH    install location; state.json, runtime/ and logs/ live here,
#                 the source checkout goes in PATH/ReStreamAir-src (default /root)
#   --bind ADDR   listening address               (default 0.0.0.0)
#   --ref REF     branch, tag or commit to build  (default main)
#   --no-start    build only, do not (re)start the server
#
# Running it again updates the checkout, rebuilds, and restarts the server
# that a previous run started. The data in --dir is never touched.
set -eu

REPO=https://github.com/testingbetaversion/ReStreamAir.git
PORT=8787
DIR=/root
BIND=0.0.0.0
REF=main
START=1

usage() {
    cat <<'EOF'
usage: install.sh [--port N] [--dir PATH] [--bind ADDR] [--ref REF] [--no-start]
  --port N      listening port (default 8787)
  --dir PATH    install location for data and source (default /root)
  --bind ADDR   listening address (default 0.0.0.0)
  --ref REF     branch, tag or commit to build (default main)
  --no-start    build only, do not (re)start the server
EOF
}
die() { echo "install.sh: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --port) [ $# -ge 2 ] || die "--port needs a value"; PORT=$2; shift 2 ;;
        --port=*) PORT=${1#*=}; shift ;;
        --dir) [ $# -ge 2 ] || die "--dir needs a value"; DIR=$2; shift 2 ;;
        --dir=*) DIR=${1#*=}; shift ;;
        --bind) [ $# -ge 2 ] || die "--bind needs a value"; BIND=$2; shift 2 ;;
        --bind=*) BIND=${1#*=}; shift ;;
        --ref) [ $# -ge 2 ] || die "--ref needs a value"; REF=$2; shift 2 ;;
        --ref=*) REF=${1#*=}; shift ;;
        --no-start) START=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

case "$PORT" in ''|*[!0-9]*) die "--port must be a number" ;; esac
[ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die "--port must be 1-65535"

if [ "$(id -u)" -eq 0 ]; then SUDO=; else SUDO=sudo; fi

# ---- build dependencies ---------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1; }
if ! need git || ! need cmake || ! need make || ! need cc; then
    echo "==> Installing build dependencies"
    if need apt-get; then
        $SUDO apt-get update -y
        $SUDO apt-get install -y git cmake build-essential pkg-config \
            libcurl4-openssl-dev libxml2-dev ca-certificates
    elif need dnf; then
        $SUDO dnf install -y git cmake gcc gcc-c++ make pkgconf-pkg-config \
            libcurl-devel libxml2-devel ca-certificates
    elif need yum; then
        $SUDO yum install -y git cmake gcc gcc-c++ make pkgconfig \
            libcurl-devel libxml2-devel ca-certificates
    elif need apk; then
        $SUDO apk add --no-cache git cmake build-base pkgconf \
            curl-dev libxml2-dev ca-certificates
    elif need pacman; then
        $SUDO pacman -Sy --noconfirm --needed git cmake base-devel pkgconf \
            curl libxml2 ca-certificates
    else
        die "no supported package manager; install git, cmake, a C/C++ compiler, libcurl and libxml2 dev files, then rerun"
    fi
fi

# ---- source ----------------------------------------------------------------
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
SRC=$DIR/ReStreamAir-src

if [ -d "$SRC/.git" ]; then
    echo "==> Updating $SRC to $REF"
    git -C "$SRC" fetch --depth 1 origin "$REF"
    git -C "$SRC" checkout -q --force FETCH_HEAD
else
    echo "==> Cloning into $SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$REPO"
    git -C "$SRC" fetch --depth 1 origin "$REF"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

# ---- build -----------------------------------------------------------------
echo "==> Building"
JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$SRC/build" -j"$JOBS"
BIN=$SRC/build/restreamair-server
[ -x "$BIN" ] || die "build finished but $BIN is missing"

[ "$START" -eq 1 ] || { echo "==> Built $BIN (not started: --no-start)"; exit 0; }

# ---- (re)start -------------------------------------------------------------
PIDFILE=$DIR/restreamair.pid
LOG=$DIR/restreamair.log
if [ -f "$PIDFILE" ]; then
    OLD=$(cat "$PIDFILE" 2>/dev/null || true)
    if [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null; then
        echo "==> Stopping previous server (pid $OLD)"
        kill "$OLD" 2>/dev/null || true
        i=0
        while kill -0 "$OLD" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
        kill -9 "$OLD" 2>/dev/null || true
    fi
fi

echo "==> Starting on $BIND:$PORT (data in $DIR)"
cd "$DIR"
nohup "$BIN" --port "$PORT" --bind "$BIND" --root "$SRC/public" >>"$LOG" 2>&1 &
echo $! >"$PIDFILE"
sleep 2
if kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "==> ReStreamAir is running: http://$BIND:$PORT  (pid $(cat "$PIDFILE"), log $LOG)"
else
    echo "==> Server exited right away; last log lines:" >&2
    tail -n 20 "$LOG" >&2
    exit 1
fi
