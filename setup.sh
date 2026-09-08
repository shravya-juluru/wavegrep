#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Shravya Juluru
#
# setup.sh - one-shot setup for wavegrep on Linux.
#
#   [1] verify platform + Python 3
#   [2] make wfm_query / shm_query executable
#   [3] build the FSDB backend (fsdb_query) if a Synopsys Verdi install is found
#   [4] check the Cadence Xcelium tools the SHM backend needs
#   [5] print what is ready and how to run it
#
# Safe to re-run. Missing vendor EDA tools are reported, not fatal: the SHM path
# works without Verdi, and the FSDB path works without Xcelium.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

# ---- defaults (override via flags, or the same env vars the tools read) ----
VERDI_HOME="${VERDI_HOME:-/tools/Synopsys/verdi/X-2025.06}"
WFM_XCELIUM_ROOT="${WFM_XCELIUM_ROOT:-/tools/Cadence/XCELIUM2409/tools.lnx86}"
DO_BUILD=1

usage() {
    cat <<EOF
usage: ./setup.sh [options]

  --verdi-home PATH     Synopsys Verdi install (has share/FsdbReader/ffrAPI.h)
                        [default: $VERDI_HOME]
  --xcelium-root PATH   Cadence Xcelium tools dir (has bin/sst2report)
                        [default: $WFM_XCELIUM_ROOT]
  --skip-build          do not compile fsdb_query (SHM-only setup)
  -h, --help            show this message

The env vars VERDI_HOME and WFM_XCELIUM_ROOT are honored when the flags are absent.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --verdi-home)   VERDI_HOME="${2:?--verdi-home needs a path}"; shift 2 ;;
        --xcelium-root) WFM_XCELIUM_ROOT="${2:?--xcelium-root needs a path}"; shift 2 ;;
        --skip-build)   DO_BUILD=0; shift ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "setup.sh: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -t 1 ]; then C_G=$'\033[32m'; C_Y=$'\033[33m'; C_R=$'\033[31m'; C_0=$'\033[0m'
else C_G=; C_Y=; C_R=; C_0=; fi
say()  { printf '     %s\n' "$*"; }
ok()   { printf '     %sok%s   %s\n'   "$C_G" "$C_0" "$*"; }
warn() { printf '     %swarn%s %s\n'   "$C_Y" "$C_0" "$*"; }
err()  { printf '     %sfail%s %s\n'   "$C_R" "$C_0" "$*"; }

fsdb_ready=0
shm_ready=0

echo "wavegrep setup"
echo

# ---- 1. platform ------------------------------------------------------------
echo "[1/5] platform"
os="$(uname -s)"; arch="$(uname -m)"
if [ "$os" != "Linux" ]; then
    warn "$os detected - the FSDB and SHM backends are Linux x86_64 only"
    say  "the VCD backend is pure Python and works here; continuing for that"
else
    [ "$arch" = "x86_64" ] || warn "expected x86_64, found $arch - the vendor libraries are x86_64 only"
    ok "$os $arch"
fi
echo

# ---- 2. python ------------------------------------------------------------
echo "[2/5] python 3"
pyver="$(python3 --version 2>&1 || true)"
if command -v python3 >/dev/null 2>&1 && printf '%s' "$pyver" | grep -q '^Python 3'; then
    ok "$pyver ($(command -v python3))"
else
    err "a working 'python3' (3.6+) is required on PATH - wfm_query and the backends need it"
    [ -n "$pyver" ] && say "got: $pyver"
    exit 1
fi
echo

# ---- 3. frontend scripts ------------------------------------------------------
echo "[3/5] frontend scripts"
chmod +x wfm_query shm_query vcd_query mcp_server.py setup.sh 2>/dev/null || true
if [ -x wfm_query ] && [ -x shm_query ] && [ -x vcd_query ]; then
    ok "wfm_query, shm_query, vcd_query are executable"
else
    warn "could not chmod +x (read-only fs?) - invoke explicitly: python3 wfm_query ..."
fi
say "VCD (.vcd/.vcd.gz) needs no vendor tools - try it now:"
say "  python3 wfm_query skills/examples/fixtures/counter.vcd info"
echo

# ---- 4. FSDB backend -------------------------------------------------------
echo "[4/5] FSDB backend (fsdb_query)"
if [ "$DO_BUILD" -eq 0 ]; then
    say "skipped (--skip-build)"
    [ -x ./fsdb_query ] && { ok "existing ./fsdb_query present"; fsdb_ready=1; }
elif [ -f "$VERDI_HOME/share/FsdbReader/ffrAPI.h" ]; then
    if ! command -v g++ >/dev/null 2>&1; then
        err "g++ not found; install a C++ toolchain, then: make VERDI_HOME=$VERDI_HOME"
    else
        say "building against $VERDI_HOME ..."
        log="$(mktemp)"
        if make VERDI_HOME="$VERDI_HOME" >"$log" 2>&1; then
            ok "built ./fsdb_query"
            fsdb_ready=1
            rm -f "$log"
        else
            err "build failed:"
            sed 's/^/       /' "$log"
            rm -f "$log"
        fi
    fi
elif [ -x ./fsdb_query ]; then
    warn "Verdi not found under $VERDI_HOME, but ./fsdb_query already exists - using it"
    fsdb_ready=1
else
    warn "Verdi FsdbReader not found under: $VERDI_HOME"
    say "--verdi-home /path/to/verdi   (needs share/FsdbReader/ffrAPI.h + LINUX64 libs)"
    say "the SHM (.shm/.trn) path does not need this"
fi
echo

# ---- 5. SHM backend toolchain -----------------------------------------------
echo "[5/5] SHM backend (Cadence Xcelium)"
missing=""
for t in sst2report simvisdbutil; do
    [ -x "$WFM_XCELIUM_ROOT/bin/$t" ] || missing="$missing $t"
done
if [ -z "$missing" ]; then
    ok "found sst2report + simvisdbutil under $WFM_XCELIUM_ROOT"
    shm_ready=1
else
    warn "missing under $WFM_XCELIUM_ROOT/bin:$missing"
    say "set WFM_XCELIUM_ROOT (or --xcelium-root) to an Xcelium tools dir"
    say "XCELIUM2209 and 2409 both provide these; the FSDB (.fsdb) path does not need this"
fi
echo

# ---- summary --------------------------------------------------------------
echo "summary"
ok   "VCD (.vcd/.vcd.gz) queries ready (no vendor tools needed)"
[ "$fsdb_ready" -eq 1 ] && ok "FSDB (.fsdb) queries ready"     || warn "FSDB (.fsdb) queries NOT ready"
[ "$shm_ready"  -eq 1 ] && ok "SHM (.shm/.trn) queries ready"  || warn "SHM (.shm/.trn) queries NOT ready"
echo

cat <<EOF
run (works regardless of the executable bit):
     python3 wfm_query <waveform> <command> [args]
     python3 wfm_query skills/examples/fixtures/counter.vcd info      # try this now

shorter form, once this setup has made them executable:
     ./wfm_query <waveform> <command> [args]

on PATH (optional):
     export PATH="$HERE:\$PATH"
EOF

if [ "$fsdb_ready" -eq 0 ] && [ "$shm_ready" -eq 0 ]; then
    echo
    echo "Note: only the VCD path is set up. Resolve the warnings above for FSDB/SHM."
fi
exit 0
