#!/usr/bin/env bash
# Executa um cenário de tests/cluster/scenarios/<nome>.sh contra o cluster do cluster.conf.
# uso: tests/cluster/run.sh <smoke|final|carga|professor>
#
# Ao terminar (bem ou mal) grava tests/cluster/out/relatorio.md e derruba o cluster.
# CLUSTER_KEEP=1 deixa tudo de pé para inspeção.

set -uo pipefail

RUN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="${CLUSTER_CONF:-$RUN_DIR/cluster.conf}"

SCENARIO="${1:-}"
if [[ -z "$SCENARIO" || ! -f "$RUN_DIR/scenarios/$SCENARIO.sh" ]]; then
    echo "uso: $0 <cenário>. Disponíveis:" >&2
    ls "$RUN_DIR/scenarios" 2>/dev/null | sed 's/\.sh$//; s/^/  /' >&2
    exit 1
fi

source "$RUN_DIR/lib/conf.sh"
source "$RUN_DIR/lib/transport.sh"
source "$RUN_DIR/lib/node.sh"
source "$RUN_DIR/lib/client.sh"

conf_load "$CONF_FILE" || exit 1
conf_validate || exit 1

source "$RUN_DIR/lib/asserts.sh"
source "$RUN_DIR/lib/steps.sh"

finish() {
    local rc=$?
    trap - EXIT INT TERM
    write_report
    if [[ -n "${CLUSTER_KEEP:-}" ]]; then
        say "CLUSTER_KEEP definido: o cluster continua de pé. Encerre com: tests/cluster/cluster.sh down"
    else
        say "encerrando o cluster"
        bash "$RUN_DIR/cluster.sh" down >/dev/null 2>&1
    fi
    exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT TERM

section "Cenário $SCENARIO — ${#HOST_LIST[@]} host(s), ${#NODE_LIST[@]} Super Peer(s), ${#CLIENT_LIST[@]} cliente(s)"
say "logs em $LOGS"

source "$RUN_DIR/scenarios/$SCENARIO.sh"

summary
