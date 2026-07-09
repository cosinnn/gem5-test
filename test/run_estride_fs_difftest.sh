#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(realpath "$(dirname "$0")/..")"
TEST_DIR="$ROOT_DIR/test"
OUT_DIR="$TEST_DIR/out_estride_fs"
ELF="$TEST_DIR/test_estride_fs.elf"
BIN="$TEST_DIR/test_estride_fs.bin"
OBJ="$TEST_DIR/test_estride_fs.o"
CONFIG="$TEST_DIR/estride_vp_fs.py"
REF_SO="${GCBV_REF_SO:-/home/cosin/xs-gem5/NEMU/build/riscv64-nemu-interpreter-so}"
GEM5_BIN="${GEM5_BIN:-$ROOT_DIR/build/RISCV/gem5.opt}"
DEBUG_FLAGS="${DEBUG_FLAGS:-VP,EStride,Fetch,Rename,IEW,Commit}"
DEBUG_FILE="${DEBUG_FILE:-estride_fs_debug.log}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

mkdir -p "$OUT_DIR"

if [[ ! -x "$GEM5_BIN" ]]; then
    printf 'gem5 binary not found or not executable: %s\n' "$GEM5_BIN" >&2
    exit 1
fi

if [[ ! -f "$REF_SO" ]]; then
    printf 'difftest reference so not found: %s\n' "$REF_SO" >&2
    exit 1
fi

printf 'Building FS test program...\n'
"${CROSS_COMPILE}as" -march=rv64imafd -mabi=lp64d "$TEST_DIR/test_estride_fs.S" -o "$OBJ"
"${CROSS_COMPILE}ld" -Ttext=0x80000000 "$OBJ" -o "$ELF"
"${CROSS_COMPILE}objcopy" -O binary "$ELF" "$BIN"

printf 'Running gem5 FS EStride difftest...\n'
printf '  gem5   : %s\n' "$GEM5_BIN"
printf '  config : %s\n' "$CONFIG"
printf '  elf    : %s\n' "$ELF"
printf '  ref so : %s\n' "$REF_SO"
printf '  outdir : %s\n' "$OUT_DIR"
printf '  debug  : %s -> %s\n' "$DEBUG_FLAGS" "$DEBUG_FILE"

(
    cd "$ROOT_DIR"
    GCBV_REF_SO="$REF_SO" "$GEM5_BIN" \
        --outdir="$OUT_DIR" \
        --debug-flags="$DEBUG_FLAGS" \
        --debug-file="$DEBUG_FILE" \
        "$CONFIG"
) | tee "$OUT_DIR/run.log"

printf '\nRun complete. Key outputs:\n'
printf '  %s\n' "$OUT_DIR/run.log"
printf '  %s\n' "$OUT_DIR/$DEBUG_FILE"
printf '  %s\n' "$OUT_DIR/stats.txt"
printf '  %s\n' "$OUT_DIR/simout"
printf '  %s\n' "$OUT_DIR/simerr"

if [[ -f "$OUT_DIR/stats.txt" ]]; then
    printf '\nVP stats summary:\n'
    rg 'VPsupported|VPpredicted|VPcorrected|VPaccuracy|VPcoverage' "$OUT_DIR/stats.txt" || true
fi

if [[ -f "$OUT_DIR/$DEBUG_FILE" ]]; then
    printf '\nEStride debug excerpts:\n'
    rg '\[ESPredict\]|\[ESUpdate\]|ValuePred-|get prediction|confidence not enough' "$OUT_DIR/$DEBUG_FILE" || true
fi
