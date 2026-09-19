#Universidade Estadual de Mato Grosso do Sul
#Curso de Ciência da Computação
#Professor Dr. Rubens Barbosa Filho
#Ano: 2015.

#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
C1DIR="$ROOT/tests/c1"
NODE="$ROOT/bin/node"
CLIENT="$ROOT/bin/client"
PORT="${C1_PORT:-55101}"
NAME="${C1_NODE_NAME:-C1SP1}"
LOG="$C1DIR/logs/node_${PORT}.log"

mkdir -p "$C1DIR/logs"

pass=0
fail=0

ok() { printf '[PASS] %s\n' "$1"; pass=$((pass+1)); }
notok() { printf '[FAIL] %s\n' "$1"; fail=$((fail+1)); }

cleanup() {
    if [[ -n "${NODE_PID:-}" ]]; then
        kill "$NODE_PID" 2>/dev/null || true
        wait "$NODE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

printf '=== CHECKPOINT C1 — TCP / PROTOCOLO / FRAMING / CRC ===\n'
printf 'Root: %s\nPort: %s\n\n' "$ROOT" "$PORT"

if [[ ! -x "$NODE" || ! -x "$CLIENT" ]]; then
    notok "Executáveis bin/node e bin/client encontrados"
    printf 'Execute: make\n'
    exit 1
fi
ok "compilado"

# 1) Unit tests do protocolo
make -C "$C1DIR" clean >/dev/null 2>&1
if make -C "$C1DIR" >/dev/null 2>&1 && "$C1DIR/test_protocol"; then
    ok "Testes unitários do protocolo"
else
    notok "Testes unitários do protocolo"
fi

# 2) Start server
rm -f "$LOG"
"$NODE" --config "$ROOT/tests/config/c1.conf" --port "$PORT" --name "$NAME" >"$LOG" 2>&1 &
NODE_PID=$!

ready=0
for _ in $(seq 1 30); do
    if "$CLIENT" --cmd ping --host 127.0.0.1 --port "$PORT" >/tmp/c1_ping.out 2>/tmp/c1_ping.err; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" -eq 1 ]]; then
    ok "Servidor TCP aceita conexões"
else
    notok "Servidor TCP aceita conexões"
    cat /tmp/c1_ping.err 2>/dev/null || true
fi

# 3) PING/PONG
if grep -q '^RX PONG' /tmp/c1_ping.out 2>/dev/null && grep -q 'PONG' /tmp/c1_ping.out; then
    ok "PING → PONG"
else
    notok "PING → PONG"
fi

# 4) JOIN/ACK
if "$CLIENT" --cmd join --host 127.0.0.1 --port "$PORT" >/tmp/c1_join.out 2>/tmp/c1_join.err && \
   grep -q '^RX ACK' /tmp/c1_join.out; then
    ok "JOIN → ACK"
else
    notok "JOIN → ACK"
fi

# 5) LEAVE/ACK (sanity test for valid message types)
if "$CLIENT" --cmd leave --host 127.0.0.1 --port "$PORT" >/tmp/c1_leave.out 2>/tmp/c1_leave.err && \
   grep -q '^RX ACK' /tmp/c1_leave.out; then
    ok "LEAVE → ACK"
else
    notok "LEAVE → ACK"
fi

# 6) Concurrent clients
pids=()
for i in $(seq 1 20); do
    "$CLIENT" --cmd ping --host 127.0.0.1 --port "$PORT" >"/tmp/c1_ping_$i.out" 2>"/tmp/c1_ping_$i.err" &
    pids+=("$!")
done
concurrent_ok=1
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then concurrent_ok=0; fi
done
if [[ "$concurrent_ok" -eq 1 ]] && [[ "$(grep -h -c '^RX PONG' /tmp/c1_ping_*.out | awk '{s+=$1} END{print s+0}')" -eq 20 ]]; then
    ok "20 clientes concorrentes"
else
    notok "20 clientes concorrentes"
fi

# 7) Server received all expected frames
rx_count=$(grep -c '^RX PING' "$LOG" 2>/dev/null || true)
if [[ "$rx_count" -ge 21 ]]; then
    ok "Framing/processamento de mensagens no servidor"
else
    notok "Framing/processamento de mensagens no servidor (RX PING=$rx_count)"
fi

# 8) Invalid header version — raw socket test implemented in Python stdlib only
python3 - "$PORT" <<'PY'
import socket, struct, sys
port = int(sys.argv[1])
# version=99; type=PING (3); zero IDs; timestamp=0; payload=0; crc=0
hdr = struct.pack('!BH', 99, 3) + bytes(32+32+16) + bytes(8+4+4)
s = socket.create_connection(('127.0.0.1', port), timeout=2)
s.sendall(hdr)
s.settimeout(2)
try:
    data = s.recv(1)
except Exception:
    data = b''
finally:
    s.close()
# Server is expected to close without accepting the malformed protocol.
sys.exit(0 if data == b'' else 1)
PY
if [[ $? -eq 0 ]]; then
    ok "Rejeição de versão de protocolo inválida"
else
    notok "Rejeição de versão de protocolo inválida"
fi

# 9) Log sanity
if grep -q "Node $NAME started" "$LOG" && grep -q 'NodeID:' "$LOG"; then
    ok "Inicialização e identificação do nó"
else
    notok "Inicialização e identificação do nó"
fi

printf '\n=== RESULTADO C1 ===\n'
printf 'PASS: %d\nFAIL: %d\n' "$pass" "$fail"
if [[ "$fail" -eq 0 ]]; then
    printf 'CHECKPOINT C1: APROVADO\n'
    exit 0
else
    printf 'CHECKPOINT C1: REPROVADO\n'
    printf 'Log do servidor: %s\n' "$LOG"
    exit 1
fi
