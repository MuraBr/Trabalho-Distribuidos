#!/usr/bin/env bash
# Painéis em tmux para o cluster: o cenário (ou o console) num painel largo, o estado
# ao lado, e uma janela com o log de cada Super Peer e outra com os clientes de cada
# máquina. Os painéis leem os logs ESPELHADOS em tests/cluster/logs, então não abrem
# conexão ssh extra. O harness em si continua sendo tests/cluster/run.sh.
#
# uso: tests/cluster/watch.sh [cenário]        (padrão: final)
#      tests/cluster/watch.sh --console        sem cenário; você comanda o cluster
#      tests/cluster/watch.sh --attach         reabre a sessão que já existe

set -euo pipefail

WATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$WATCH_DIR/../.." && pwd)"
LOG_DIR="$WATCH_DIR/logs"
SESSION="${CLUSTER_TMUX_SESSION:-bittorrent-cluster}"
CONF_FILE="${CLUSTER_CONF:-$WATCH_DIR/cluster.conf}"

source "$WATCH_DIR/lib/conf.sh"
conf_load "$CONF_FILE" || exit 1
conf_validate || exit 1

if ! command -v tmux >/dev/null 2>&1; then
    echo "tmux não encontrado" >&2
    exit 1
fi

MODE="${1:-final}"

if [[ "$MODE" == "--attach" ]]; then
    exec tmux attach -t "$SESSION"
fi

if [[ -n "${TMUX:-}" ]]; then
    echo "Já está dentro do tmux. Saia desta sessão e rode de novo." >&2
    exit 1
fi

mkdir -p "$LOG_DIR"

if tmux has-session -t "$SESSION" 2>/dev/null; then
    tmux kill-session -t "$SESSION"
fi

if [[ "$MODE" == "--console" ]]; then
    main_cmd="bash tests/cluster/cluster.sh console; echo; echo \"[console] encerrou\"; read"
else
    main_cmd="CLUSTER_WATCH=1 bash tests/cluster/run.sh $(printf '%q' "$MODE"); echo; echo \"[harness] encerrou com \$?\"; read"
fi

status_cmd='while true; do clear; bash tests/cluster/cluster.sh status 2>&1; sleep 3; done'

tail_cmd() {
    local title="$1" file="$2"
    printf '%s' "echo $(printf '%q' "=== $title ==="); exec tail -n +1 -F -- $(printf '%q' "$file")"
}

# Um tail por cliente da máquina, com o nome na frente de cada linha.
clients_cmd() {
    local h="$1" name cmd=""
    cmd="echo $(printf '%q' "=== clientes de $h ($(clients_on_host "$h" | wc -l)) ==="); "
    while read -r name; do
        [[ -n "$name" ]] || continue
        cmd+="( tail -n 0 -F -- $(printf '%q' "$LOG_DIR/$name.log") 2>/dev/null | sed -u 's/^/[$name] /' ) & "
    done < <(clients_on_host "$h")
    cmd+="wait"
    printf '%s' "$cmd"
}

tmux new-session -d -s "$SESSION" -n fluxo -x 200 -y 60 -c "$ROOT" "$main_cmd"
tmux select-pane -t "$SESSION:fluxo" -T "harness"
tmux split-window -h -t "$SESSION:fluxo" -c "$ROOT" "$status_cmd"
tmux select-pane -t "$SESSION:fluxo.1" -T "estado"
tmux select-pane -t "$SESSION:fluxo.0"
tmux select-layout -t "$SESSION:fluxo" main-vertical

# Janela com um painel por Super Peer.
first=1
for name in "${NODE_LIST[@]}"; do
    title="$name@${NODE_HOST[$name]} $(node_endpoint "$name")"
    if ((first)); then
        tmux new-window -t "$SESSION" -n sps -c "$ROOT" "$(tail_cmd "$title" "$LOG_DIR/$name.log")"
        first=0
    else
        tmux split-window -t "$SESSION:sps" -c "$ROOT" "$(tail_cmd "$title" "$LOG_DIR/$name.log")"
        tmux select-layout -t "$SESSION:sps" tiled
    fi
    tmux select-pane -t "$SESSION:sps" -T "$title"
done
tmux select-layout -t "$SESSION:sps" tiled

# Janela com um painel por máquina, juntando os clientes dela.
first=1
for h in "${HOST_LIST[@]}"; do
    [[ -n "$(clients_on_host "$h")" ]] || continue
    if ((first)); then
        tmux new-window -t "$SESSION" -n clientes -c "$ROOT" "$(clients_cmd "$h")"
        first=0
    else
        tmux split-window -t "$SESSION:clientes" -c "$ROOT" "$(clients_cmd "$h")"
        tmux select-layout -t "$SESSION:clientes" tiled
    fi
    tmux select-pane -t "$SESSION:clientes" -T "clientes@$h"
done
((first)) || tmux select-layout -t "$SESSION:clientes" tiled

# Console para digitar comandos enquanto o cenário roda.
if [[ "$MODE" != "--console" ]]; then
    tmux new-window -t "$SESSION" -n console -c "$ROOT" "bash tests/cluster/cluster.sh console; read"
fi

tmux set-option -t "$SESSION" pane-border-status top
tmux set-option -t "$SESSION" pane-border-format ' #{pane_title} '
tmux select-window -t "$SESSION:fluxo"

if [[ -t 1 ]]; then
    exec tmux attach -t "$SESSION"
fi

echo "Sessão $SESSION pronta. Anexe com: tmux attach -t $SESSION"
