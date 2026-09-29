# Curso de Ciência da Computação 
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

section "C2 — arquivo, SHA-256, chunks e LZ4"
require_bin "$NODE_BIN" || exit 1
require_bin "$CLIENT_BIN" || exit 1
if [[ -x "$TEST_ROOT/generate_fixtures.sh" ]]; then
    "$TEST_ROOT/generate_fixtures.sh" >/dev/null || exit 1
elif [[ ! -d "$DATA_DIR" && -f "$TEST_ROOT/data.tar.gz" ]]; then
    tar -xzf "$TEST_ROOT/data.tar.gz" -C "$TEST_ROOT" || exit 1
fi

PDFS=()
for f in "$DATA_DIR"/*.pdf "$PROJECT_ROOT"/*.pdf; do
    [[ -f "$f" ]] || continue
    if head -c 1024 "$f" | LC_ALL=C grep -a -q '%PDF-'; then
        PDFS+=("$f")
    else
        log "Ignorando $(basename "$f"): não contém assinatura PDF nos primeiros 1024 bytes"
    fi
done
if [[ "${#PDFS[@]}" -eq 0 ]]; then
    fail "Nenhum PDF válido encontrado em $DATA_DIR ou $PROJECT_ROOT"
    summary
    exit 1
fi

RUN_DIR="$(mktemp -d "$LOG_DIR/c2-pdf.XXXXXX")" || exit 1
LOG="$RUN_DIR/c2.log"
DOWNLOAD_DIR="$RUN_DIR/downloads"
mkdir -p "$DOWNLOAD_DIR"

SUPERPEER_PORT="${C2_PORT:-55301}"
PEER_PORT="${C2_PEER_PORT:-55302}"
cd "$PROJECT_ROOT" || exit 1
"$NODE_BIN" --config "$CONFIG_DIR/c1.conf" --port "$SUPERPEER_PORT" --name C2SP1 >"$LOG" 2>&1 &
PIDS+=("$!")
if ! wait_for_pattern "$LOG" '^Node C2SP1 started$' 10; then
    fail "Super Peer não iniciou; consulte $LOG"
    summary
    exit 1
fi

"$CLIENT_BIN" serve "$PEER_PORT" 127.0.0.1 "$SUPERPEER_PORT" >>"$LOG" 2>&1 &
PIDS+=("$!")
if ! wait_for_pattern "$LOG" '^Peer storage started$' 10; then
    fail "Peer de armazenamento não iniciou; consulte $LOG"
    summary
    exit 1
fi

for f in "${PDFS[@]}"; do
    name="$(basename "$f")"
    if "$CLIENT_BIN" upload "$f" 127.0.0.1 "$PEER_PORT" >>"$LOG" 2>&1; then
        ok "Upload de $name"
    else
        fail "Upload de $name"
        continue
    fi
    if "$CLIENT_BIN" download "$name" "$DOWNLOAD_DIR/$name" 127.0.0.1 "$SUPERPEER_PORT" >>"$LOG" 2>&1; then
        ok "Download de $name"
        assert_file_equal "$f" "$DOWNLOAD_DIR/$name"
    else
        fail "Download de $name"
    fi
done

assert_contains "$LOG" 'SHA-256|ObjectID' "Há evidência de SHA-256/ObjectID"
assert_contains "$LOG" 'LZ4|compress' "Há evidência de compressão LZ4"
assert_contains "$LOG" 'chunk|Chunk' "Há evidência de fragmentação"
assert_no_crash "$LOG"

printf 'Log: %s\n' "$LOG"
summary
