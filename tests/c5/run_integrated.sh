#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$(mktemp -d /tmp/pd-c5-bin.XXXXXX)}"
CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'

echo "C5 integrado: build e testes estritos"
echo "Configuração: $PROJECT_ROOT/tests/config/c5.conf"
echo "Binários e logs da integração: $BIN_DIR"
make -C "$PROJECT_ROOT" BIN_DIR="$BIN_DIR" test-c5 CFLAGS="$CFLAGS"
