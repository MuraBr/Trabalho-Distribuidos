#!/usr/bin/env bash
# Controla o cluster de teste (Super Peers e clientes em várias máquinas, via ssh).
# A topologia vem de tests/cluster/cluster.conf. `cluster.sh help` lista os comandos.

set -uo pipefail

CLUSTER_SH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="${CLUSTER_CONF:-$CLUSTER_SH_DIR/cluster.conf}"

# shellcheck source=lib/conf.sh
source "$CLUSTER_SH_DIR/lib/conf.sh"
# shellcheck source=lib/transport.sh
source "$CLUSTER_SH_DIR/lib/transport.sh"
# shellcheck source=lib/node.sh
source "$CLUSTER_SH_DIR/lib/node.sh"
# shellcheck source=lib/client.sh
source "$CLUSTER_SH_DIR/lib/client.sh"

usage() {
    cat <<'EOF'
uso: tests/cluster/cluster.sh <comando> [args]

Preparo
  list                     mostra a topologia gerada pelo cluster.conf
  copy-keys [--per-host]   copia a chave ssh para as máquinas (pede a senha, não grava)
  check                    confere conf, ssh, compilador, bibliotecas, relógio e portas
  sync                     envia o código para as máquinas
  build                    compila (make) em cada máquina
  fixtures                 gera e envia os arquivos de teste (run/files)
  deploy                   sync + build + fixtures

Execução
  up [--clients] [--keep]  zera o estado, sobe os Super Peers (e os clientes)
  down                     fecha clientes, encerra Super Peers e espelhos de log
  status [--clients]       estado de cada processo
  kill|stop|start|restart <nome>   Super Peer ou cliente (kill = SIGKILL)
  net                      testa o alcance TCP de cada máquina a cada Super Peer

Clientes (ficam abertos até serem fechados)
  client start [nomes...]  sobe clientes (padrão: todos)
  client do <nome> <comando...>    envia, espera o [done] e mostra a saída
  client send <nome> <comando...>  envia sem esperar; imprime o número do comando
  client wait <nome> <N> [seg]     espera o comando N e devolve o rc dele
  client batch <nome> <arquivo>    manda um arquivo de comandos
  client attach <nome>     digita direto na sessão do cliente
  client stop [nomes...]   envia quit
  client status            lista os clientes

Observação
  console                  laço interativo (C1 upload x | @SP2 kill | status)
  logs <nome>              acompanha o log espelhado de um processo
  watch [--console]        painéis em tmux
  run <cenário>            executa scenarios/<cenário>.sh (smoke, final, carga)
  exec <host> <cmd...>     roda um comando na máquina
  clean [--data]           apaga logs locais (com --data, também o estado remoto)

Variáveis: CLUSTER_CONF (padrão tests/cluster/cluster.conf), SSHPASS (senha para copy-keys).
EOF
}

die_cli() { echo "erro: $*" >&2; exit 1; }

load_conf() {
    conf_load "$CONF_FILE" || exit 1
    conf_validate || exit 1
}

cmd_list() {
    local h name n
    echo "Hosts (${#HOST_LIST[@]}):"
    for h in "${HOST_LIST[@]}"; do
        printf '  %-6s %-28s ip=%-15s dir=%s  SPs=%s clientes=%s\n' "$h" "${HOST_TARGET[$h]}" "${HOST_IP[$h]}" \
            "${HOST_DIR[$h]}" "$(nodes_on_host "$h" | wc -l)" "$(clients_on_host "$h" | wc -l)"
    done
    echo "Super Peers (${#NODE_LIST[@]}):"
    for name in "${NODE_LIST[@]}"; do
        printf '  %-6s %-6s %-21s prioridade %s\n' "$name" "${NODE_HOST[$name]}" "$(node_endpoint "$name")" "${NODE_PRIO[$name]}"
    done
    echo "Clientes (${#CLIENT_LIST[@]}): ${CLIENT_LIST[0]:-}..${CLIENT_LIST[-1]:-} (use 'client status' para o detalhe)"
    echo "Eleição: vence o maior NodeID anunciado pelo programa"
}

cmd_check() {
    local rc=0
    echo "cluster.conf: ${#HOST_LIST[@]} host(s), ${#NODE_LIST[@]} Super Peer(s), ${#CLIENT_LIST[@]} cliente(s) — ok"
    _check_host() {
        local h="$1" out ports p t0 t1 skew
        t0="$(date +%s.%N)"
        out="$(rexec_t 20 "$h" 'echo ok; command -v gcc >/dev/null && echo gcc=ok || echo gcc=FALTA;
            command -v make >/dev/null && echo make=ok || echo make=FALTA;
            command -v stdbuf >/dev/null && echo stdbuf=ok || echo stdbuf=FALTA;
            command -v setsid >/dev/null && echo setsid=ok || echo setsid=FALTA;
            l="$( (ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null) | cat)";
            for lib in libcrypto liblz4 libz.so; do grep -q "$lib" <<<"$l" && echo "$lib=ok" || echo "$lib=FALTA"; done;
            echo "now=$(date +%s.%N)"; echo "cpus=$(nproc)"; ss -ltn 2>/dev/null | awk "NR>1{print \$4}" | sed "s/.*://" | tr "\n" " " | sed "s/^/listen=/"; echo' 2>&1)" || {
            echo "ssh falhou em ${HOST_TARGET[$h]}: ${out:-sem resposta}. Rode: tests/cluster/cluster.sh copy-keys"
            return 1
        }
        t1="$(date +%s.%N)"
        local bad=0 line now listen
        while IFS= read -r line; do
            case "$line" in
            *=FALTA) echo "falta: ${line%=FALTA}"; bad=1 ;;
            now=*) now="${line#now=}" ;;
            listen=*) listen="${line#listen=}" ;;
            esac
        done <<<"$out"
        if [[ -n "${now:-}" ]]; then
            skew="$(awk -v a="$t0" -v b="$t1" -v r="$now" 'BEGIN { m = (a + b) / 2; d = r - m; if (d < 0) d = -d; printf "%.1f", d }')"
            echo "relógio: diferença de ~${skew}s em relação a esta máquina"
            awk -v d="$skew" 'BEGIN { exit !(d > 2) }' && echo "AVISO: relógio com mais de 2 s de diferença (o header das mensagens usa o relógio de cada nó)"
        fi
        for p in $(nodes_on_host "$h" | while read -r n; do echo "${NODE_PORT[$n]}"; done); do
            [[ " ${listen:-} " == *" $p "* ]] && { echo "porta $p já está em uso"; bad=1; }
        done
        [[ $bad -eq 0 ]] && echo "ok (ssh, ferramentas, bibliotecas, portas livres)"
        return "$bad"
    }
    par_hosts _check_host || rc=1
    return "$rc"
}

cmd_sync() {
    _sync() { sync_code "$1" && echo "código enviado"; }
    rm -f "$STATE"/abs.*
    par_hosts _sync
}

cmd_build() {
    _build() {
        local h="$1" abs
        abs="$(host_abs "$h")" || return 1
        rexec_t 900 "$h" "cd '$abs' && make -j\$(nproc) all 2>&1 | tail -n 5; test -x bin/node -a -x bin/client" && echo "compilado"
    }
    par_hosts _build
}

cmd_fixtures() {
    make_fixtures
    _fx() { push_fixtures "$1" && echo "arquivos de teste enviados"; }
    par_hosts _fx
}

cmd_net() {
    local rc=0
    _net() {
        local h="$1" name list out
        list="$(for name in "${NODE_LIST[@]}"; do printf '%s %s %s\n' "$(node_ip "$name")" "${NODE_PORT[$name]}" "$name"; done)"
        out="$(rexec_t 90 "$h" "while read -r ip port name; do
timeout 3 bash -c \"exec 3<>/dev/tcp/\$ip/\$port\" 2>/dev/null || echo \"sem alcance: \$name \$ip:\$port\"
done <<'LIST'
$list
LIST" 2>&1)"
        if [[ -n "$out" ]]; then
            printf '%s\n' "$out"
            return 1
        fi
        echo "alcança os ${#NODE_LIST[@]} Super Peers"
    }
    par_hosts _net || rc=1
    return "$rc"
}

cmd_up() {
    local with_clients=0 keep=0 a h
    for a in "$@"; do
        case "$a" in
        --clients) with_clients=1 ;;
        --keep) keep=1 ;;
        *) die_cli "opção desconhecida: $a" ;;
        esac
    done
    for h in "${HOST_LIST[@]}"; do
        rexec_t 20 "$h" "test -x \"$(host_dir_expr "$h")/bin/node\" -a -x \"$(host_dir_expr "$h")/bin/client\"" ||
            die_cli "bin/node ou bin/client faltando em $h. Rode: cluster.sh deploy"
    done
    if ((!keep)); then
        say "zerando o estado anterior"
        cluster_reset
    fi
    _prep() { prepare_host "$1" && echo "roster e configs enviados"; }
    par_hosts _prep || exit 1
    nodes_up || exit 1
    cmd_net || echo "AVISO: alguma máquina não alcança algum Super Peer (firewall?)"
    ((with_clients)) && clients_start
    return 0
}

cmd_down() {
    local h name
    say "fechando clientes"
    for h in "${HOST_LIST[@]}"; do
        local -a names=()
        mapfile -t names < <(clients_on_host "$h")
        [[ ${#names[@]} -gt 0 ]] || continue
        rrun "$h" send-lines < <(for name in "${names[@]}"; do echo "$name quit"; done) 2>/dev/null || true
    done
    sleep 1
    for h in "${HOST_LIST[@]}"; do
        local -a all=()
        mapfile -t all < <(procs_on_host "$h")
        rrun "$h" stop "${all[@]}" 2>/dev/null || true
    done
    for h in "${HOST_LIST[@]}"; do mirror_stop "$h"; done
    ssh_close_all
    say "tudo encerrado"
}

cmd_status() {
    nodes_status
    if [[ "${1:-}" == "--clients" ]]; then
        echo
        clients_status
    else
        local h alive=0 total=0 n
        for h in "${HOST_LIST[@]}"; do
            local -a names=()
            mapfile -t names < <(clients_on_host "$h")
            [[ ${#names[@]} -gt 0 ]] || continue
            total=$((total + ${#names[@]}))
            n="$(rrun "$h" alive "${names[@]}" 2>/dev/null | grep -c ' alive ' || true)"
            alive=$((alive + n))
        done
        echo
        echo "clientes: $alive/$total abertos"
    fi
    for h in "${HOST_LIST[@]}"; do
        mirror_alive "$h" || echo "espelho de log de $h: parado"
    done
    return 0
}

proc_op() {
    local op="$1" name="$2" h
    if [[ -n "${NODE_HOST[$name]:-}" ]]; then
        case "$op" in
        kill) node_kill "$name" ;;
        stop) node_stop "$name" ;;
        start) node_start "$name" ;;
        restart) node_stop "$name"; node_start "$name" ;;
        esac
    elif [[ -n "${CLIENT_HOST[$name]:-}" ]]; then
        h="${CLIENT_HOST[$name]}"
        case "$op" in
        kill) rrun "$h" kill9 "$name" ;;
        stop) client_stop "$name" ;;
        start) clients_start "$name" ;;
        restart) client_stop "$name"; clients_start "$name" ;;
        esac
    else
        die_cli "processo desconhecido: $name"
    fi
}

cmd_client() {
    local sub="${1:-}" n
    shift || true
    case "$sub" in
    start) clients_start "$@" ;;
    stop) clients_stop "$@" ;;
    status) clients_status ;;
    do)
        [[ $# -ge 2 ]] || die_cli "uso: client do <nome> <comando...>"
        local name="$1" rc
        shift
        n="$(client_send "$name" "$@")" || exit 1
        client_wait "$name" "$n" "${CLIENT_TIMEOUT:-60}"
        rc=$?
        client_output "$name" "$n"
        echo "[rc=$rc]"
        exit "$rc"
        ;;
    send)
        [[ $# -ge 2 ]] || die_cli "uso: client send <nome> <comando...>"
        client_send "$@"
        ;;
    wait)
        [[ $# -ge 2 ]] || die_cli "uso: client wait <nome> <N> [segundos]"
        client_wait "$@"
        ;;
    batch)
        [[ $# -eq 2 ]] || die_cli "uso: client batch <nome> <arquivo>"
        client_batch "$@"
        ;;
    attach)
        [[ $# -eq 1 ]] || die_cli "uso: client attach <nome>"
        client_attach "$1"
        ;;
    *) die_cli "client: subcomando desconhecido: ${sub:-vazio}" ;;
    esac
}

# Laço interativo. "C1 upload x", "C1,C2 lookup x", "@SP2 kill" e qualquer comando do cluster.sh.
cmd_console() {
    local line first rest name n rc
    local -a targets
    local histfile="$STATE/console.history"
    history -r "$histfile" 2>/dev/null || true
    echo "console do cluster. 'help' lista comandos; Ctrl-D sai (não derruba o cluster)."
    while IFS= read -er -p "cluster> " line; do
        [[ -n "${line// /}" ]] || continue
        history -s "$line"
        history -w "$histfile" 2>/dev/null || true
        first="${line%% *}"
        rest="${line#"$first"}"
        rest="${rest# }"
        case "$first" in
        quit | exit) break ;;
        help) usage ;;
        @*)
            name="${first#@}"
            ( proc_op "${rest:-status}" "$name" ) </dev/null 2>&1 || true
            ;;
        *)
            IFS=',' read -ra targets <<<"$first"
            if [[ -n "${CLIENT_HOST[${targets[0]}]:-}" && -n "$rest" ]]; then
                # </dev/null: o ssh dos comandos internos não pode comer a entrada do console
                local -A sent=()
                for name in "${targets[@]}"; do
                    sent[$name]="$(client_send "$name" "$rest" </dev/null)" || continue
                done
                for name in "${!sent[@]}"; do
                    client_wait "$name" "${sent[$name]}" "${CLIENT_TIMEOUT:-60}" </dev/null
                    rc=$?
                    echo "--- $name"
                    client_output "$name" "${sent[$name]}"
                    echo "[rc=$rc]"
                done
            else
                if [[ "$first $rest" == "client attach"* ]]; then
                    echo "client attach precisa do terminal inteiro: rode fora do console"
                else
                    # shellcheck disable=SC2086
                    ( dispatch $line ) </dev/null 2>&1 || true
                fi
            fi
            ;;
        esac
    done
}

cmd_logs() {
    [[ $# -eq 1 ]] || die_cli "uso: logs <nome>"
    exec tail -n +1 -F "$(node_log "$1")"
}

cmd_clean() {
    local h
    if [[ "${1:-}" == "--data" ]]; then
        for h in "${HOST_LIST[@]}"; do
            local -a all=()
            mapfile -t all < <(procs_on_host "$h")
            rrun "$h" wipe "${all[@]}" 2>/dev/null || true
        done
    fi
    rm -rf "$LOGS"/* "$OUT/state" "$OUT/stage" "$OUT/relatorio.md" 2>/dev/null
    mkdir -p "$STATE" "$SYNC_DIR"
    say "limpo"
}

dispatch() {
    local cmd="${1:-help}"
    shift || true
    case "$cmd" in
    help | -h | --help) usage ;;
    list) cmd_list ;;
    copy-keys) copy_keys "$@" ;;
    check) cmd_check ;;
    sync) cmd_sync ;;
    build) cmd_build ;;
    fixtures) cmd_fixtures ;;
    deploy) cmd_sync && cmd_build && cmd_fixtures ;;
    up) cmd_up "$@" ;;
    down) cmd_down ;;
    status) cmd_status "$@" ;;
    net) cmd_net ;;
    kill | stop | start | restart)
        [[ $# -ge 1 ]] || die_cli "uso: $cmd <nome>..."
        local n
        for n in "$@"; do proc_op "$cmd" "$n"; done
        ;;
    client) cmd_client "$@" ;;
    console) cmd_console ;;
    logs) cmd_logs "$@" ;;
    watch) exec bash "$CLUSTER_SH_DIR/watch.sh" "$@" ;;
    run) exec bash "$CLUSTER_SH_DIR/run.sh" "$@" ;;
    exec)
        [[ $# -ge 2 ]] || die_cli "uso: exec <host> <comando...>"
        local h="$1"
        shift
        [[ -n "${HOST_TARGET[$h]:-}" ]] || die_cli "host desconhecido: $h"
        rexec_t 120 "$h" "$*"
        ;;
    clean) cmd_clean "$@" ;;
    _mirror) mirror_run "$1" ;;
    *) usage; exit 1 ;;
    esac
}

case "${1:-help}" in
help | -h | --help) usage; exit 0 ;;
esac

load_conf
dispatch "$@"
