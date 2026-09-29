# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash

# Ajuste esta variável para a raiz do projeto.
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

NODE_BIN="${NODE_BIN:-$PROJECT_ROOT/bin/node}"
CLIENT_BIN="${CLIENT_BIN:-$PROJECT_ROOT/bin/client}"

TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$TEST_ROOT/data"
CONFIG_DIR="$TEST_ROOT/config"
if [[ ! -f "$CONFIG_DIR/c1.conf" && -f "$PROJECT_ROOT/tests/config/c1.conf" ]]; then
    CONFIG_DIR="$PROJECT_ROOT/tests/config"
fi
LOG_DIR="${LOG_DIR:-/tmp/bittorrent-tests}"

# Contrato temporal do PDF
HEARTBEAT_SEC="${HEARTBEAT_SEC:-5}"
FAILURE_TIMEOUT_SEC="${FAILURE_TIMEOUT_SEC:-15}"

# O PDF exige chunking de 4 MB.
CHUNK_SIZE="${CHUNK_SIZE:-4194304}"

# Quantidade de nós usada nos testes
SUPERPEERS="${SUPERPEERS:-5}"
PEERS="${PEERS:-3}"

export PROJECT_ROOT NODE_BIN CLIENT_BIN TEST_ROOT DATA_DIR CONFIG_DIR LOG_DIR
export HEARTBEAT_SEC FAILURE_TIMEOUT_SEC CHUNK_SIZE SUPERPEERS PEERS

mkdir -p "$LOG_DIR"
