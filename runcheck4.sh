#Curso de Ciência da Computação
#Professor Dr. Rubens Barbosa Filho
#Ano: 2015

#!/usr/bin/env bash
set -u
source "$(dirname "$0")/../common.sh"

section "C4 — Bully + SMR"
require_bin "$NODE_BIN" || exit 1

LOG="$LOG_DIR/c4.log"
: > "$LOG"

for port in 5101 5102 5103 5104; do
    "$NODE_BIN" --config "$CONFIG_DIR/c4.conf" --port "$port" >>"$LOG" 2>&1 &
    PIDS+=("$!")
done
sleep 8

# O último nó deve ser o de maior prioridade na configuração.
# Primeiro derruba o coordenador esperado (SP4).
kill -9 "${PIDS[3]}" 2>/dev/null || true
sleep $((FAILURE_TIMEOUT_SEC + 4))

if [[ -x "$CLIENT_BIN" ]]; then
    "$CLIENT_BIN" --cmd put --name c4-file --value value1 --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
    "$CLIENT_BIN" --cmd state --host 127.0.0.1 --port 5101 >>"$LOG" 2>&1 || true
fi

assert_contains "$LOG" 'ELECTION|Election' "Eleição foi iniciada"
assert_contains "$LOG" 'COORDINATOR|Coordinator' "Novo coordenador foi anunciado"
assert_contains "$LOG" 'SMR|log|LogIndex|checkpoint' "Há evidência de SMR/log"
assert_no_crash "$LOG"
summary
