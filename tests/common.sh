# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash
set -u

source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

PASS=0
FAIL=0
PIDS=()

log()  { printf '[TEST] %s\n' "$*"; }
ok()   { printf '\033[32m[PASS]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
fail() { printf '\033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
section() { printf '\n==== %s ====\n' "$*"; }

cleanup() {
    for p in "${PIDS[@]:-}"; do
        kill "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

require_bin() {
    local b="$1"
    if [[ ! -x "$b" ]]; then
        fail "Executável não encontrado: $b"
        return 1
    fi
}

wait_for_pattern() {
    local file="$1" pattern="$2" timeout="$3"
    local start now
    start=$(date +%s)
    while true; do
        grep -Eq "$pattern" "$file" 2>/dev/null && return 0
        now=$(date +%s)
        (( now - start >= timeout )) && return 1
        sleep 1
    done
}

assert_file_equal() {
    local a="$1" b="$2"
    if cmp -s "$a" "$b"; then
        ok "Arquivos são idênticos: $(basename "$a")"
    else
        fail "Arquivos diferem: $a x $b"
    fi
}

assert_contains() {
    local file="$1" pattern="$2" desc="$3"
    if grep -Eq "$pattern" "$file"; then
        ok "$desc"
    else
        fail "$desc — padrão não encontrado: $pattern"
    fi
}

assert_no_crash() {
    local file="$1"
    if grep -Eiq 'segmentation fault|core dumped|double free|heap-buffer-overflow|deadlock' "$file"; then
        fail "Falha de execução detectada em $(basename "$file")"
    else
        ok "Sem crash/deadlock reportado em $(basename "$file")"
    fi
}

summary() {
    echo
    echo "=============================="
    echo "PASS: $PASS"
    echo "FAIL: $FAIL"
    echo "=============================="
    [[ "$FAIL" -eq 0 ]]
}
