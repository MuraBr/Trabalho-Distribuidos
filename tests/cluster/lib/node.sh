#!/usr/bin/env bash
# Ciclo de vida dos Super Peers e o espelho de log. Cada processo remoto escreve
# em run/<nome>/proc.log; um processo local por host lê todos esses logs de uma vez
# (ssh tail -F), carimba cada linha com o relógio desta máquina e grava em
# logs/<nome>.<vida>.log. logs/<nome>.log aponta para a vida mais recente.

# Algumas locales usam vírgula como separador decimal; o EPOCHREALTIME e o awk herdam isso.
export LC_NUMERIC=C

CLUSTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOGS="$CLUSTER_DIR/logs"
OUT="$CLUSTER_DIR/out"
STATE="$OUT/state"
SYNC_DIR="$STATE/sync"
mkdir -p "$LOGS" "$OUT" "$STATE" "$SYNC_DIR"

say() { printf '%s [cluster] %s\n' "$(date +%H:%M:%S.%3N)" "$*"; }

# Caminho absoluto do projeto no host (cria o diretório se preciso). Fica em cache.
host_abs() {
    local h="$1" f="$STATE/abs.$1" abs
    if [[ -s "$f" ]]; then
        cat "$f"
        return 0
    fi
    abs="$(rexec "$h" "mkdir -p \"$(host_dir_expr "$h")\" && cd \"$(host_dir_expr "$h")\" && pwd")" || return 1
    if [[ "$abs" != /* ]]; then
        echo "host_abs: não consegui resolver o diretório do projeto em $h (saída: '$abs')" >&2
        return 1
    fi
    printf '%s' "$abs" >"$f"
    printf '%s' "$abs"
}

# Roda remote.sh no host. Os argumentos são citados para a shell remota.
rrun() {
    local h="$1" abs
    shift
    abs="$(host_abs "$h")" || return 1
    rexec "$h" "bash '$abs/run/remote.sh' '$abs/run' $(printf '%q ' "$@")"
}

rrun_stream() {
    local h="$1" abs
    shift
    abs="$(host_abs "$h")" || return 1
    rexec_stream "$h" "bash '$abs/run/remote.sh' '$abs/run' $(printf '%q ' "$@")"
}

# --- preparo ------------------------------------------------------------

# Gera roster e .conf dos clientes e os envia (com remote.sh) para run/ de cada host.
prepare_host() {
    local h="$1" stage="$OUT/stage/$1" c abs
    abs="$(host_abs "$h")" || return 1
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$CLUSTER_DIR/lib/remote.sh" "$stage/remote.sh"
    conf_write_roster "$stage/roster.conf"
    while read -r c; do
        [[ -n "$c" ]] || continue
        mkdir -p "$stage/$c"
        conf_write_client_conf "$c" "$stage/$c/peer.conf"
    done < <(clients_on_host "$h")
    push_tree "$h" "$stage" "$abs/run"
}

# Cria os arquivos de teste e os envia para run/files de cada host.
make_fixtures() {
    local dir="$OUT/files" l
    mkdir -p "$dir"
    for l in a b c d e; do
        {
            printf 'Documento %s\nLinha repetida para testar compressão.\n%.0s' "$l" {1..500}
        } >"$dir/documento_${l}.pdf"
    done
    # 9 MiB: três chunks de 4 MiB.
    [[ -s "$dir/documento_big.pdf" ]] || head -c $((9 * 1024 * 1024)) /dev/urandom >"$dir/documento_big.pdf"
}

push_fixtures() {
    local h="$1" abs
    abs="$(host_abs "$h")" || return 1
    push_tree "$h" "$OUT/files" "$abs/run/files"
}

# --- espelho de log -----------------------------------------------------

mirror_pidfile() { printf '%s/mirror.%s.pid' "$STATE" "$1"; }

mirror_alive() {
    local f pid
    f="$(mirror_pidfile "$1")"
    [[ -s "$f" ]] || return 1
    pid="$(cat "$f")"
    kill -0 "$pid" 2>/dev/null
}

# Lê a saída de `tail -v -n +1 -F` e distribui por processo, com carimbo local.
mirror_demux() {
    local name="" line path life file tok t secs ms blank=0
    local -A cur=()

    while IFS= read -r line; do
        if [[ "$line" == "==> "*" <==" ]]; then
            path="${line#==> }"
            path="${path% <==}"
            name="${path%/proc.log}"
            name="${name##*/}"
            blank=0 # a linha em branco antes do cabeçalho é do tail, não do programa
            continue
        fi
        [[ -n "$name" ]] || continue
        if ((blank)); then
            blank=0
            file="${cur[$name]:-}"
            [[ -n "$file" ]] && printf '%s \n' "$(_stamp)" >>"$file"
        fi
        if [[ -z "$line" ]]; then
            blank=1
            continue
        fi
        if [[ "$line" == "### life "* ]]; then
            life="${line#\#\#\# life }"
            file="$LOGS/$name.$life.log"
            : >"$file"
            ln -sfn "$name.$life.log" "$LOGS/$name.log"
            cur[$name]="$file"
            continue
        fi
        if [[ "$line" == "### sync "* ]]; then
            tok="${line#\#\#\# sync }"
            : >"$SYNC_DIR/$tok.$name"
            continue
        fi
        file="${cur[$name]:-}"
        [[ -n "$file" ]] || continue
        t="${EPOCHREALTIME/,/.}"
        secs="${t%.*}"
        ms="${t#*.}"
        printf '%(%H:%M:%S)T.%s %s\n' "$secs" "${ms:0:3}" "$line" >>"$file"
    done
}

_stamp() {
    local t="${EPOCHREALTIME/,/.}" ms
    ms="${t#*.}"
    printf '%(%H:%M:%S)T.%s' "${t%.*}" "${ms:0:3}"
}

# Corpo do processo de espelho (rodado em segundo plano por mirror_start).
mirror_run() {
    local h="$1" names
    mapfile -t names < <(procs_on_host "$h")
    ( rrun_stream "$h" tail "${names[@]}" ) | mirror_demux
}

mirror_start() {
    local h="$1" names pid
    # Vários nós do mesmo host sobem em paralelo e cada um pede o espelho; sem o lock
    # nasceriam espelhos duplicados, cada um regravando os logs.
    (
        flock 9
        mirror_alive "$h" && exit 0
        mapfile -t names < <(procs_on_host "$h")
        rrun "$h" touch "${names[@]}" || exit 1
        # setsid: o espelho continua depois que este comando do cluster.sh termina.
        setsid bash "$CLUSTER_DIR/cluster.sh" _mirror "$h" >/dev/null 2>&1 9>&- &
        printf '%s' "$!" >"$(mirror_pidfile "$h")"
    ) 9>"$STATE/mirror.$h.lock"
}

mirror_stop() {
    local h="$1" f pid
    f="$(mirror_pidfile "$h")"
    [[ -s "$f" ]] || return 0
    pid="$(cat "$f")"
    kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
    rm -f "$f"
}

# Espera os logs locais de $@ alcançarem o que os processos já escreveram.
mirror_sync() {
    local tok name h deadline missing
    local -A by_host=()
    tok="$$.$RANDOM$RANDOM"
    for name in "$@"; do
        h="$(proc_host "$name")"
        by_host[$h]+="$name "
    done
    for h in "${!by_host[@]}"; do
        # shellcheck disable=SC2086
        rrun "$h" token "$tok" ${by_host[$h]} || return 1
    done
    deadline=$((SECONDS + 15))
    while ((SECONDS < deadline)); do
        missing=0
        for name in "$@"; do
            [[ -e "$SYNC_DIR/$tok.$name" ]] || missing=1
        done
        if ((!missing)); then
            rm -f "$SYNC_DIR/$tok".*
            return 0
        fi
        sleep 0.1
    done
    rm -f "$SYNC_DIR/$tok".*
    echo "mirror_sync: o espelho de log não alcançou ($*)" >&2
    return 1
}

# --- Super Peers --------------------------------------------------------

node_log() { printf '%s/%s.log' "$LOGS" "$1"; }

node_start() {
    local name="$1" h abs
    h="${NODE_HOST[$name]}"
    abs="$(host_abs "$h")" || return 1
    mirror_start "$h" || return 1
    rrun "$h" start "$name" -- "$abs/bin/node" --config "$abs/run/roster.conf" --port "${NODE_PORT[$name]}"
}

node_stop() { rrun "${NODE_HOST[$1]}" stop "$1"; }

node_kill() {
    say "kill -9 $1 (${NODE_HOST[$1]} $(node_endpoint "$1"))"
    rrun "${NODE_HOST[$1]}" kill9 "$1"
}

node_alive() { rrun "${NODE_HOST[$1]}" alive "$1" >/dev/null; }

# Zera o estado de uma execução anterior e prepara os hosts. Não sobe nada.
cluster_reset() {
    local h name
    for h in "${HOST_LIST[@]}"; do mirror_stop "$h"; done
    rm -f "$LOGS"/*.log "$SYNC_DIR"/* 2>/dev/null
    rm -f "$STATE"/*.sent 2>/dev/null
    for h in "${HOST_LIST[@]}"; do
        local -a names
        mapfile -t names < <(procs_on_host "$h")
        rrun "$h" wipe "${names[@]}" 2>/dev/null || true
    done
}

# Sobe os Super Peers dados (padrão: todos). O primeiro da lista sobe antes dos
# outros, que o roster usa como bootstrap.
nodes_up() {
    local -a names=("$@") rest=()
    local first n
    [[ ${#names[@]} -gt 0 ]] || names=("${NODE_LIST[@]}")
    first="${names[0]}"
    # espelhos de todos os hosts antes de subir qualquer nó, em série
    for n in "${names[@]}"; do mirror_start "${NODE_HOST[$n]}" || return 1; done
    say "subindo $first (bootstrap)"
    node_start "$first" >/dev/null || return 1
    for n in "${names[@]:1}"; do rest+=("$n"); done
    local -a pids=()
    for n in "${rest[@]}"; do
        ( node_start "$n" >/dev/null ) &
        pids+=("$!")
    done
    local rc=0 p
    for p in "${pids[@]}"; do wait "$p" || rc=1; done
    [[ $rc -eq 0 ]] || return 1
    # O log espelhado de cada nó só existe depois que o espelho leu o marcador de vida.
    local end=$((SECONDS + 20))
    for n in "${names[@]}"; do
        until [[ -e "$(node_log "$n")" ]]; do
            ((SECONDS < end)) || { echo "log de $n não apareceu (espelho parado?)" >&2; return 1; }
            sleep 0.2
        done
    done
    say "${#names[@]} Super Peer(s) no ar"
}

nodes_status() {
    local h name line
    printf '%-8s %-6s %-21s %-6s %s\n' NOME HOST ENDERECO ESTADO VIDA
    for h in "${HOST_LIST[@]}"; do
        local -a names
        mapfile -t names < <(nodes_on_host "$h")
        [[ ${#names[@]} -gt 0 ]] || continue
        while read -r name state pid life; do
            printf '%-8s %-6s %-21s %-6s %s\n' "$name" "$h" "$(node_endpoint "$name")" "$state" "$life"
        done < <(rrun "$h" alive "${names[@]}" 2>/dev/null || true)
    done
}
