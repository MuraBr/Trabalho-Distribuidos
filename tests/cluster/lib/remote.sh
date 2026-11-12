#!/usr/bin/env bash
# Roda NA MÁQUINA REMOTA (é copiado para <projeto>/run/remote.sh).
# Cuida do ciclo de vida de um processo por nome. Cada processo tem o seu
# diretório <run>/<nome>/, que é também o diretório de trabalho dele: o storage
# do Super Peer (data/storage/<porta>) e do cliente (storage/) é relativo a ele.
#
# uso: remote.sh <run-dir> <subcomando> [args...]
#   start <nome> -- <cmd...>          sobe um Super Peer em segundo plano
#   start-client <nome> -- <cmd...>   sobe um cliente com stdin = <nome>/in.fifo
#   alive <nome>...                   imprime "<nome> alive|dead <pid> <vida>"
#   stop <nome>...                    SIGTERM, depois SIGKILL se não sair em 5 s
#   kill9 <nome>...                   SIGKILL
#   send <nome> <linha>               escreve a linha na stdin do cliente
#   send-lines                        lê "<nome> <linha>" da stdin e envia cada uma
#   touch <nome>...                   cria <nome>/proc.log (para o tail -F)
#   tail <nome>...                    tail -n +1 -F nos logs (espelho de log)
#   token <token> <nome>...           anota "### sync <token>" no fim dos logs
#   wipe <nome>...                    apaga o diretório do processo
#   sha256 <arquivo>                  sha256 do arquivo

set -u

RUN="${1:?run-dir}"
CMD="${2:?subcomando}"
shift 2

mkdir -p "$RUN"

pid_of() { cat "$RUN/$1/pid" 2>/dev/null; }

is_alive() {
    local pid
    pid="$(pid_of "$1")"
    [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

start_proc() {
    local mode="$1" name="$2" pid life
    shift 2
    [[ "${1:-}" == "--" ]] && shift
    mkdir -p "$RUN/$name"
    if is_alive "$name"; then
        echo "$name já está rodando (pid $(pid_of "$name"))" >&2
        return 1
    fi
    life=$(( $(cat "$RUN/$name/life" 2>/dev/null || echo 0) + 1 ))
    echo "$life" >"$RUN/$name/life"
    # O marcador separa as vidas no log; o espelho local troca de arquivo nele.
    printf '### life %d\n' "$life" >>"$RUN/$name/proc.log"
    cd "$RUN/$name" || return 1
    if [[ "$mode" == client ]]; then
        rm -f in.fifo
        mkfifo in.fifo
        # O fd 3 é aberto para leitura E escrita: o cliente nunca recebe EOF
        # enquanto estiver vivo, e quem escreve no FIFO nunca bloqueia.
        exec 3<>in.fifo
        setsid nohup stdbuf -oL -eL "$@" <&3 >>proc.log 2>&1 3<&- &
        pid=$!
        exec 3<&-
    else
        setsid nohup stdbuf -oL -eL "$@" </dev/null >>proc.log 2>&1 &
        pid=$!
    fi
    echo "$pid" >pid
    sleep 0.2
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "$name morreu logo ao subir (ver $RUN/$name/proc.log)" >&2
        return 1
    fi
    echo "$name pid $pid vida $life"
}

stop_proc() {
    local name="$1" pid i
    pid="$(pid_of "$name")"
    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    kill -TERM "$pid" 2>/dev/null
    for ((i = 0; i < 50; i++)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null
}

case "$CMD" in
start) start_proc node "$@" ;;
start-client) start_proc client "$@" ;;
alive)
    rc=0
    for n in "$@"; do
        if is_alive "$n"; then
            echo "$n alive $(pid_of "$n") $(cat "$RUN/$n/life" 2>/dev/null || echo 0)"
        else
            echo "$n dead $(pid_of "$n") $(cat "$RUN/$n/life" 2>/dev/null || echo 0)"
            rc=1
        fi
    done
    exit "$rc"
    ;;
stop)
    for n in "$@"; do stop_proc "$n"; done
    ;;
kill9)
    for n in "$@"; do
        pid="$(pid_of "$n")"
        [[ -n "$pid" ]] && kill -KILL "$pid" 2>/dev/null
    done
    exit 0
    ;;
send)
    name="$1"
    shift
    if ! is_alive "$name"; then
        echo "$name não está rodando" >&2
        exit 1
    fi
    printf '%s\n' "$*" >"$RUN/$name/in.fifo"
    ;;
send-lines)
    while IFS= read -r line; do
        name="${line%% *}"
        text="${line#* }"
        if is_alive "$name"; then
            printf '%s\n' "$text" >"$RUN/$name/in.fifo"
        else
            echo "$name não está rodando" >&2
        fi
    done
    ;;
touch)
    for n in "$@"; do
        mkdir -p "$RUN/$n"
        touch "$RUN/$n/proc.log"
    done
    ;;
tail)
    files=()
    for n in "$@"; do files+=("$RUN/$n/proc.log"); done
    exec tail -n +1 -F "${files[@]}" 2>/dev/null
    ;;
token)
    tok="$1"
    shift
    for n in "$@"; do
        printf '### sync %s\n' "$tok" >>"$RUN/$n/proc.log"
    done
    ;;
wipe)
    for n in "$@"; do
        stop_proc "$n"
        rm -rf "${RUN:?}/$n"
    done
    ;;
sha256)
    sha256sum "$1" | awk '{print $1}'
    ;;
*)
    echo "subcomando desconhecido: $CMD" >&2
    exit 2
    ;;
esac
