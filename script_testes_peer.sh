#!/usr/bin/env bash

# Teste de integração usando somente o executável construído a partir de peer.c.
# No Makefile, bin/client é apenas um alias para esse mesmo executável.
set -u

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PEER="$ROOT/bin/node"
LOG_DIR="$ROOT/tests/peer/logs"
PORT_A="${C1_PEER_PORT:-55201}"
CONCURRENT="${C1_PEER_CONCURRENT:-3}"
PING_CLIENTS="${C1_PEER_PINGS:-20}"

pass=0
fail=0
pids=()

mkdir -p "$LOG_DIR"

ok()
{
    printf '[PASS] %s\n' "$1"
    pass=$((pass + 1))
}

notok()
{
    printf '[FAIL] %s\n' "$1"
    fail=$((fail + 1))
}

cleanup()
{
    local pid

    for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
}

trap cleanup EXIT INT TERM

wait_for_log()
{
    local log_file="$1"
    local pattern="$2"
    local attempt

    for attempt in $(seq 1 50); do
        if grep -Eq -- "$pattern" "$log_file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_peer()
{
    local log_file="$1"
    shift

    stdbuf -oL -eL "$PEER" "$@" >"$log_file" 2>&1 &
    pids+=("$!")
}

printf '=== TESTE DE INTEGRACAO — PEER COM PEER ===\n'
printf 'Root: %s\nPorta inicial: %s\nPeers concorrentes: %s\nPINGs concorrentes: %s\n\n' "$ROOT" "$PORT_A" "$CONCURRENT" "$PING_CLIENTS"

if ! make -C "$ROOT" bin/node >/dev/null; then
    notok 'compilação do peer'
    exit 1
fi
ok 'peer compilado (sem executar bin/client)'

if make -C "$ROOT/tests/c1" >/dev/null 2>&1 && "$ROOT/tests/c1/test_protocol"; then
    ok 'teste unitário do protocolo'
else
    notok 'teste unitário do protocolo'
fi

LOG_A="$LOG_DIR/peer_a_${PORT_A}.log"
rm -f "$LOG_A"
start_peer "$LOG_A" --config "$ROOT/tests/config/c1.conf" --port "$PORT_A" --name A

if wait_for_log "$LOG_A" '^Node A started$'; then
    ok 'Peer A iniciou o servidor TCP'
else
    notok 'Peer A iniciou o servidor TCP'
fi

if grep -Eq '^NodeID: [0-9a-f]{64}$' "$LOG_A" 2>/dev/null; then
    ok 'Peer A exibiu um NodeID de 32 bytes'
else
    notok 'Peer A exibiu um NodeID de 32 bytes'
fi

PING_OUT="$LOG_DIR/ping.out"
PING_ERR="$LOG_DIR/ping.err"
if "$PEER" --cmd ping --host 127.0.0.1 --port "$PORT_A" >"$PING_OUT" 2>"$PING_ERR" && grep -q '^TX PING$' "$PING_OUT" && grep -q '^RX PONG$' "$PING_OUT"; then
    ok 'PING com payload textual recebeu PONG textual'
else
    notok 'PING com payload textual recebeu PONG textual'
fi

SHORT_PING_OUT="$LOG_DIR/ping_short.out"
SHORT_PING_ERR="$LOG_DIR/ping_short.err"
if "$PEER" -c ping -h 127.0.0.1 -p "$PORT_A" >"$SHORT_PING_OUT" 2>"$SHORT_PING_ERR" && grep -q '^TX PING$' "$SHORT_PING_OUT" && grep -q '^RX PONG$' "$SHORT_PING_OUT"; then
    ok 'opções curtas -c, -h e -p processadas por getopt_long'
else
    notok 'opções curtas -c, -h e -p processadas por getopt_long'
fi

LEAVE_OUT="$LOG_DIR/leave.out"
LEAVE_ERR="$LOG_DIR/leave.err"
if "$PEER" --cmd leave --host 127.0.0.1 --port "$PORT_A" >"$LEAVE_OUT" 2>"$LEAVE_ERR" && grep -q '^TX LEAVE$' "$LEAVE_OUT" && grep -q '^RX ACK$' "$LEAVE_OUT"; then
    ok 'LEAVE recebeu ACK'
else
    notok 'LEAVE recebeu ACK'
fi

if [[ "$PING_CLIENTS" =~ ^[1-9][0-9]*$ ]]; then
    rm -f "$LOG_DIR"/ping_concurrent_*.out "$LOG_DIR"/ping_concurrent_*.err
    command_pids=()
    index=1
    while [[ "$index" -le "$PING_CLIENTS" ]]; do
        "$PEER" --cmd ping --host 127.0.0.1 --port "$PORT_A" >"$LOG_DIR/ping_concurrent_${index}.out" 2>"$LOG_DIR/ping_concurrent_${index}.err" &
        command_pids+=("$!")
        index=$((index + 1))
    done

    concurrent_ping_ok=1
    for pid in "${command_pids[@]}"; do
        if ! wait "$pid"; then
            concurrent_ping_ok=0
        fi
    done
    pong_count=$(grep -h -c '^RX PONG$' "$LOG_DIR"/ping_concurrent_*.out | awk '{sum += $1} END {print sum + 0}')
    if [[ "$concurrent_ping_ok" -eq 1 && "$pong_count" -eq "$PING_CLIENTS" ]]; then
        ok "Peer A respondeu a ${PING_CLIENTS} PINGs concorrentes"
    else
        notok "Peer A respondeu a ${PING_CLIENTS} PINGs concorrentes (PONGs=$pong_count)"
    fi

    expected_pings=$((PING_CLIENTS + 2))
    if wait_for_log "$LOG_A" "^RX PING$" && [[ "$(grep -c '^RX PING$' "$LOG_A" 2>/dev/null || true)" -ge "$expected_pings" ]]; then
        ok 'framing dos PINGs concorrentes no servidor'
    else
        notok 'framing dos PINGs concorrentes no servidor'
    fi
else
    notok 'valor de C1_PEER_PINGS'
fi

PORT_B=$((PORT_A + 1))
LOG_B="$LOG_DIR/peer_b_${PORT_B}.log"
rm -f "$LOG_B"

# A forma posicional já existente em peer.c inicia o servidor local e envia JOIN.
start_peer "$LOG_B" "$PORT_B" 127.0.0.1 "$PORT_A"

if wait_for_log "$LOG_A" 'JOIN validado:.*membros=2'; then
    ok 'Peer A recebeu, validou e registrou o JOIN de Peer B'
else
    notok 'Peer A recebeu, validou e registrou o JOIN de Peer B'
fi

if wait_for_log "$LOG_B" 'JOIN aceito pelo peer remoto:.*membros=2'; then
    ok 'Peer B recebeu ACK e registrou Peer A'
else
    notok 'Peer B recebeu ACK e registrou Peer A'
fi

if grep -Eq '^NodeID: [0-9a-f]{64}$' "$LOG_B" 2>/dev/null; then
    ok 'Peer B exibiu um NodeID de 32 bytes'
else
    notok 'Peer B exibiu um NodeID de 32 bytes'
fi

if [[ "$CONCURRENT" =~ ^[1-9][0-9]*$ ]]; then
    index=1
    while [[ "$index" -le "$CONCURRENT" ]]; do
        port=$((PORT_A + 1 + index))
        log_file="$LOG_DIR/peer_${index}_${port}.log"
        rm -f "$log_file"
        start_peer "$log_file" "$port" 127.0.0.1 "$PORT_A"
        index=$((index + 1))
    done

    expected_members=$((2 + CONCURRENT))
    if wait_for_log "$LOG_A" "JOIN validado:.*membros=${expected_members}"; then
        ok "Peer A processou ${CONCURRENT} JOINs concorrentes"
    else
        notok "Peer A processou ${CONCURRENT} JOINs concorrentes"
    fi
else
    notok 'valor de C1_PEER_CONCURRENT'
fi

join_count=$(grep -c '^JOIN validado:' "$LOG_A" 2>/dev/null || true)
if [[ "$join_count" -ge $((CONCURRENT + 1)) ]]; then
    ok 'framing e processamento de mensagens entre peers'
else
    notok "framing e processamento de mensagens entre peers (JOINs=$join_count)"
fi

printf '\n=== RESULTADO PEER COM PEER ===\n'
printf 'PASS: %d\nFAIL: %d\n' "$pass" "$fail"

if [[ "$fail" -eq 0 ]]; then
    printf 'TESTE PEER: APROVADO\n'
    exit 0
fi

printf 'TESTE PEER: REPROVADO\n'
printf 'Logs: %s\n' "$LOG_DIR"
exit 1
