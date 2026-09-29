#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
SP_PORT="${C2_SUPERPEER_PORT:-55101}"
PEER_A_PORT="${C2_PEER_A_PORT:-55102}"
PEER_B_PORT="${C2_PEER_B_PORT:-55103}"
TMP="$(mktemp -d)"
SP_PID=""
PEER_A_PID=""
PEER_B_PID=""
EXECUTOR_PORT="$PEER_A_PORT"
peer_cmd() { "$ROOT/bin/peer" --local-peer-port "$EXECUTOR_PORT" "$@"; }
PASS=0
FAIL=0

pass() { printf '[PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf '[FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }

stop_pid() {
    local pid="${1:-}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_pid "$PEER_A_PID"
    EXECUTOR_PORT="$PEER_A_PORT"
    stop_pid "$PEER_B_PID"
    stop_pid "$SP_PID"
    if (( FAIL > 0 )); then
        printf 'Evidências preservadas: %s\n' "$TMP"
    elif [[ -n "$TMP" && "$TMP" == /tmp/tmp.* && -d "$TMP" ]]; then
        rm -rf -- "$TMP"
    fi
}
trap cleanup EXIT INT TERM

wait_port() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 80); do
        if (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

cd "$ROOT" || exit 1
printf '=== CHECKPOINT C2 — ALUNO 1 ===\nRoot: %s\n\n' "$ROOT"

if make -B CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror' >/dev/null; then
    pass 'bin/peer, bin/superpeer, bin/node e bin/client compilados com -Werror'
else
    fail 'compilação'
    exit 1
fi
if make CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror' test-c2 >/dev/null; then
    pass 'CRC, truncamento, payload excessivo, chunks inválidos e ObjectID divergente são rejeitados'
else
    fail 'testes negativos de protocolo e armazenamento'
fi

cd "$TMP" || exit 1
"$ROOT/bin/superpeer" --port "$SP_PORT" --name superpeer >"$TMP/superpeer.log" 2>&1 &
SP_PID=$!
if wait_port "$SP_PORT"; then pass 'Super Peer iniciou'; else fail 'Super Peer iniciou'; exit 1; fi

"$ROOT/bin/peer" serve "$PEER_A_PORT" 127.0.0.1 "$SP_PORT" >"$TMP/peer-a.log" 2>&1 &
PEER_A_PID=$!
"$ROOT/bin/peer" serve "$PEER_B_PORT" 127.0.0.1 "$SP_PORT" >"$TMP/peer-b.log" 2>&1 &
PEER_B_PID=$!
if wait_port "$PEER_A_PORT" && wait_port "$PEER_B_PORT"; then pass 'dois Peers de armazenamento iniciaram e fizeram JOIN'; else fail 'Peers iniciaram'; exit 1; fi

SMALL="$ROOT/trabalho_2026_SD.pdf"
if PEER_TRANSFER_THREADS=4 peer_cmd upload "$SMALL" 127.0.0.1 "$PEER_A_PORT" >"$TMP/upload-small.log" 2>&1 && grep -q 'Upload completed' "$TMP/upload-small.log" && grep -Eq '^ObjectID: [0-9a-f]{64}$' "$TMP/upload-small.log"; then
    pass 'upload de PDF pequeno com ObjectID e saída obrigatória'
else
    fail 'upload de PDF pequeno'
fi
SMALL_ID="$(sed -n 's/^ObjectID: //p' "$TMP/upload-small.log" | head -n 1)"

{
    printf '%%PDF-1.4\n'
    dd if=/dev/zero bs=1048576 count=4 status=none
    dd if=/dev/zero bs=1 count=4096 status=none
    printf '\n%%%%EOF\n'
} >"$TMP/multichunk.pdf"
if PEER_TRANSFER_THREADS=4 peer_cmd upload "$TMP/multichunk.pdf" 127.0.0.1 "$PEER_A_PORT" >"$TMP/upload-large.log" 2>&1 && grep -q '^Chunks: 2$' "$TMP/upload-large.log" && grep -q '^Transfer workers: 2$' "$TMP/upload-large.log"; then
    pass 'PDF multichunk transferido com dois workers configurados'
else
    fail 'upload multichunk concorrente'
fi
LARGE_ID="$(sed -n 's/^ObjectID: //p' "$TMP/upload-large.log" | head -n 1)"
if "$ROOT/bin/peer" benchmark "$TMP/multichunk.pdf" >"$TMP/benchmark.log" 2>&1 && grep -Eq '^Compression throughput: [0-9]+\.[0-9]+ MiB/s$' "$TMP/benchmark.log"; then pass 'benchmark LZ4 informativo executado sem limite rígido'; else fail 'benchmark LZ4'; fi

if peer_cmd download trabalho_2026_SD.pdf "$TMP/by-name.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && cmp -s "$SMALL" "$TMP/by-name.pdf"; then pass 'download por nome e comparação byte a byte'; else fail 'download por nome'; fi
if peer_cmd download "$LARGE_ID" "$TMP/by-id.pdf" 127.0.0.1 "$SP_PORT" >"$TMP/download-id.log" 2>&1 && cmp -s "$TMP/multichunk.pdf" "$TMP/by-id.pdf" && grep -q 'SHA-256 verified' "$TMP/download-id.log"; then pass 'download por ObjectID e SHA-256 final'; else fail 'download por ObjectID'; fi

mkdir -p "$TMP/nome-a" "$TMP/nome-b"
printf '%%PDF-1.4\nobjeto A\n%%%%EOF\n' >"$TMP/nome-a/ambiguo.pdf"
printf '%%PDF-1.4\nobjeto B\n%%%%EOF\n' >"$TMP/nome-b/ambiguo.pdf"
if peer_cmd upload "$TMP/nome-a/ambiguo.pdf" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1 && peer_cmd upload "$TMP/nome-b/ambiguo.pdf" 127.0.0.1 "$PEER_B_PORT" >/dev/null 2>&1 && ! peer_cmd download ambiguo.pdf "$TMP/ambiguo.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1; then pass 'nome ambíguo exige ObjectID'; else fail 'detecção de nome ambíguo'; fi

if peer_cmd upload "$SMALL" 127.0.0.1 "$PEER_B_PORT" >/dev/null 2>&1; then pass 'registro de uma segunda localização para os chunks'; else fail 'segunda localização'; fi
stop_pid "$PEER_A_PID"
PEER_A_PID=""
EXECUTOR_PORT="$PEER_B_PORT"
if peer_cmd download "$SMALL_ID" "$TMP/fallback.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && cmp -s "$SMALL" "$TMP/fallback.pdf"; then pass 'download pela localização remanescente após LEAVE'; else fail 'download pela localização remanescente após LEAVE'; fi

"$ROOT/bin/peer" serve "$PEER_A_PORT" 127.0.0.1 "$SP_PORT" >"$TMP/peer-a-restart.log" 2>&1 &
PEER_A_PID=$!
if wait_port "$PEER_A_PORT"; then
    FIRST_NODE_ID="$(sed -n 's/^NodeID: //p' "$TMP/peer-a.log" | head -n 1)"
    RESTART_NODE_ID="$(sed -n 's/^NodeID: //p' "$TMP/peer-a-restart.log" | head -n 1)"
    if [[ -n "$FIRST_NODE_ID" && "$FIRST_NODE_ID" == "$RESTART_NODE_ID" ]]; then pass 'NodeID do Peer permanece estável após reinicialização'; else fail 'persistência da identidade do Peer'; fi
    EXECUTOR_PORT="$PEER_A_PORT"
    stop_pid "$PEER_B_PID"
    PEER_B_PID=""
    if peer_cmd download "$LARGE_ID" "$TMP/after-restart.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && cmp -s "$TMP/multichunk.pdf" "$TMP/after-restart.pdf"; then pass 'reinicialização e leitura do catálogo local'; else fail 'novo anúncio após reinicialização'; fi
else
    fail 'reinicialização do Peer'
fi

if peer_cmd upload "$SMALL" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1; then pass 'upload repetido idempotente'; else fail 'upload repetido idempotente'; fi
printf 'arquivo de texto existente\\n' >"$TMP/not-pdf.txt"
if ! peer_cmd upload "$TMP/not-pdf.txt" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1; then pass 'arquivo não PDF rejeitado'; else fail 'arquivo não PDF rejeitado'; fi
printf 'conteudo sem assinatura PDF\n' >"$TMP/falso.pdf"
if peer_cmd upload "$TMP/falso.pdf" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1; then pass 'fixture sintética .pdf aceita por extensão'; else fail 'fixture sintética .pdf aceita'; fi
if ! peer_cmd upload "$SMALL" 127.0.0.1 1 >/dev/null 2>&1; then pass 'porta indisponível gera erro'; else fail 'falha de conexão propagada'; fi
if ! peer_cmd download "$SMALL_ID" "$TMP/after-restart.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1; then pass 'destino existente não é sobrescrito'; else fail 'destino existente não é sobrescrito'; fi
printf 'sentinela de download parcial\n' >"$TMP/owned.part"
if ! peer_cmd download "$SMALL_ID" "$TMP/owned" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && grep -qx 'sentinela de download parcial' "$TMP/owned.part"; then pass 'arquivo .part preexistente é preservado'; else fail 'arquivo .part preexistente'; fi

printf '\n=== RESULTADO C2 ===\nPASS: %d\nFAIL: %d\n' "$PASS" "$FAIL"
if (( FAIL == 0 )); then
    printf 'CHECKPOINT C2: APROVADO\n'
    exit 0
fi
printf 'CHECKPOINT C2: REPROVADO\n'
exit 1
