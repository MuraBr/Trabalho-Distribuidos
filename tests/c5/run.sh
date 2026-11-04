#Curso de Ciência da Computação
#Prof. Dr. Rubens Barbosa Filho
#2015

#!/usr/bin/env bash
set -u
source "$(dirname "$0")/../common.sh"

section "C5 — 2PC + IST + LFU"
require_bin "$NODE_BIN" || exit 1
"$TEST_ROOT/generate_fixtures.sh" >/dev/null

LOG="$LOG_DIR/c5.log"
: > "$LOG"

for port in 5101 5102 5103; do
    "$NODE_BIN" --config "$CONFIG_DIR/c5.conf" --port "$port" >>"$LOG" 2>&1 &
    PIDS+=("$!")
done
sleep 8

if [[ -x "$CLIENT_BIN" ]]; then
    "$CLIENT_BIN" --cmd upload --file "$DATA_DIR/documento_a.pdf" >>"$LOG" 2>&1 || true
    "$CLIENT_BIN" --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
    "$CLIENT_BIN" --cmd lookup --name documento_a.pdf --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
fi

assert_contains "$LOG" 'PREPARE|Prepare' "Há fase PREPARE"
assert_contains "$LOG" 'COMMIT|ABORT' "Há decisão COMMIT/ABORT"
assert_contains "$LOG" 'LFU|frequency|cache|CACHE' "Há evidência de LFU/cache"

# Simula entrada de nó atrasado; a implementação pode expor JOIN/STATE_TRANSFER
# por sua própria CLI. O teste verifica a evidência nos logs.
if [[ -x "$CLIENT_BIN" ]]; then
    "$CLIENT_BIN" --cmd join --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
fi

assert_contains "$LOG" 'IST|STATE_TRANSFER|checkpoint|version' "Há evidência de IST/sincronização"
assert_no_crash "$LOG"
summary
