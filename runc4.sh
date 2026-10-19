#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

printf '=== CHECKPOINT C4 — Bully + log replicado ===\n'
make -B CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'
make test-c4-local
python3 tests/c4/integration.py --bin-dir bin
printf 'RESULTADO C4: execução concluída sem falhas.\n'
