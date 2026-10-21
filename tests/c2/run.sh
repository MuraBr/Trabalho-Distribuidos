# Curso de Ciência da Computação 
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash
set -u
source "$(dirname "$0")/../common.sh"

section "C2 — arquivo, SHA-256, chunks e LZ4"
require_bin "$NODE_BIN" || exit 1
"$TEST_ROOT/generate_fixtures.sh" >/dev/null

LOG="$LOG_DIR/c2.log"
DOWNLOAD_DIR="$LOG_DIR/c2-download"
mkdir -p "$DOWNLOAD_DIR"

"$NODE_BIN" --config "$CONFIG_DIR/c1.conf" >"$LOG" 2>&1 &
PIDS+=("$!")
sleep 2

if [[ ! -x "$CLIENT_BIN" ]]; then
    fail "CLIENT_BIN não encontrado"
    summary; exit 1
fi

for f in small.txt exact_4MiB.bin over_4MiB.bin random_10MiB.bin; do
    "$CLIENT_BIN" --cmd upload --file "$DATA_DIR/$f" >>"$LOG" 2>&1 || true
    "$CLIENT_BIN" --cmd download --name "$f" --output "$DOWNLOAD_DIR/$f" >>"$LOG" 2>&1 || true
done

assert_contains "$LOG" 'SHA-256|ObjectID' "Há evidência de SHA-256/ObjectID"
assert_contains "$LOG" 'LZ4|compress' "Há evidência de compressão LZ4"
assert_contains "$LOG" 'chunk|Chunk' "Há evidência de fragmentação"
assert_no_crash "$LOG"

for f in small.txt exact_4MiB.bin over_4MiB.bin random_10MiB.bin; do
    [[ -f "$DOWNLOAD_DIR/$f" ]] && assert_file_equal "$DATA_DIR/$f" "$DOWNLOAD_DIR/$f"
done
summary
