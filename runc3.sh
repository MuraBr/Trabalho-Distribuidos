# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015


#!/usr/bin/env bash
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

section "C3 — Chord + Gossip"
require_bin "$NODE_BIN" || exit 1

LOG="$LOG_DIR/c3.log"
: > "$LOG"

# Convenção de CLI: --node-id é opcional se ocorre a derivação NodeID da config.
for port in 5101 5102 5103 5104 5105; do
    "$NODE_BIN" --config "$CONFIG_DIR/c3.conf" --port "$port" >>"$LOG" 2>&1 &
    PIDS+=("$!")
done

sleep 8

if [[ -x "$CLIENT_BIN" ]]; then
    "$CLIENT_BIN" --cmd topology --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
    "$CLIENT_BIN" --cmd lookup --object-id "$(printf 'ab%.0s' {1..32})" --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
fi

assert_contains "$LOG" 'successor|Successor' "Há successor no overlay"
assert_contains "$LOG" 'finger|Finger' "Há finger table"
assert_contains "$LOG" 'GOSSIP|Gossip|heartbeat|HEARTBEAT' "Há Gossip/heartbeat"

# Derruba SP3 e aguarda mais que o timeout especificado.
kill -9 "${PIDS[2]}" 2>/dev/null || true
sleep $((FAILURE_TIMEOUT_SEC + 3))

assert_contains "$LOG" 'SUSPECT|FAILED|REMOVED|Election|ELECTION' \
    "Há evidência de detecção de falha após timeout"
assert_no_crash "$LOG"
summary
