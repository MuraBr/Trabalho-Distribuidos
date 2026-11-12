#!/usr/bin/env bash
# Lê tests/cluster/cluster.conf e gera os .conf que o código já entende:
# o roster dos Super Peers e um .conf de peer para cada cliente.
#
# Formato (uma diretiva por linha, '#' começa comentário):
#   set <chave> <valor...>
#   host <apelido> <alvo-ssh|local> [ip=IP] [dir=DIR] [sps=N] [clients=N] [sp_ports=P1,P2,..]
#   node <nome> superpeer <host> <porta> [prioridade]      (Super Peer avulso)
#   client <nome> <host> [super-peer-de-entrada] [port=P]   (cliente avulso)
#
# Cada host gera sozinho `sps` Super Peers (padrão: set sp_per_host) e `clients`
# clientes (padrão: set clients_per_host). Os nomes são globais: SP1..SPn e C1..Cm.

declare -gA CL_SET=(
    [ssh_opts]="-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5"
    [remote_dir]='~/bittorrent-project'
    [build]="remote"
    [sp_per_host]=3
    [clients_per_host]=30
    [sp_port_base]=5100
    [client_port_base]=9000
)

HOST_LIST=()
declare -gA HOST_TARGET HOST_IP HOST_DIR HOST_SPS HOST_CLIENTS HOST_SPPORTS
NODE_LIST=()
declare -gA NODE_HOST NODE_PORT NODE_PRIO
CLIENT_LIST=()
declare -gA CLIENT_HOST CLIENT_PORT CLIENT_SP

# Limite do parser de roster (CONFIG_ROSTER_MAX em include/common/config.h).
ROSTER_MAX=64

conf_err() { printf 'cluster.conf: %s\n' "$*" >&2; }

is_ipv4() { [[ "$1" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; }

# IP de um alvo ssh: o trecho depois do '@', resolvendo nome quando preciso.
target_ip() {
    local target="$1" h
    h="${target##*@}"
    if is_ipv4 "$h"; then
        printf '%s' "$h"
        return 0
    fi
    getent ahostsv4 "$h" 2>/dev/null | awk 'NR==1 {print $1}'
}

conf_load() {
    local file="$1" line key
    local -a tok
    local lineno=0 opt

    [[ -f "$file" ]] || { conf_err "arquivo não encontrado: $file (copie cluster.conf.example)"; return 1; }

    while IFS= read -r line || [[ -n "$line" ]]; do
        lineno=$((lineno + 1))
        line="${line%%#*}"
        read -ra tok <<<"$line"
        [[ ${#tok[@]} -eq 0 ]] && continue
        case "${tok[0]}" in
        set)
            [[ ${#tok[@]} -ge 3 ]] || { conf_err "linha $lineno: set <chave> <valor>"; return 1; }
            key="${tok[1]}"
            CL_SET[$key]="${tok[*]:2}"
            ;;
        host)
            [[ ${#tok[@]} -ge 3 ]] || { conf_err "linha $lineno: host <apelido> <alvo-ssh|local>"; return 1; }
            local alias="${tok[1]}"
            [[ -z "${HOST_TARGET[$alias]:-}" ]] || { conf_err "linha $lineno: host repetido: $alias"; return 1; }
            HOST_LIST+=("$alias")
            HOST_TARGET[$alias]="${tok[2]}"
            HOST_IP[$alias]=""
            HOST_DIR[$alias]=""
            HOST_SPS[$alias]=""
            HOST_CLIENTS[$alias]=""
            HOST_SPPORTS[$alias]=""
            for opt in "${tok[@]:3}"; do
                case "$opt" in
                ip=*) HOST_IP[$alias]="${opt#ip=}" ;;
                dir=*) HOST_DIR[$alias]="${opt#dir=}" ;;
                sps=*) HOST_SPS[$alias]="${opt#sps=}" ;;
                clients=*) HOST_CLIENTS[$alias]="${opt#clients=}" ;;
                sp_ports=*) HOST_SPPORTS[$alias]="${opt#sp_ports=}" ;;
                *) conf_err "linha $lineno: opção desconhecida em host: $opt"; return 1 ;;
                esac
            done
            ;;
        node)
            [[ ${#tok[@]} -ge 5 && "${tok[2]}" == superpeer ]] ||
                { conf_err "linha $lineno: node <nome> superpeer <host> <porta> [prioridade]"; return 1; }
            NODE_LIST+=("${tok[1]}")
            NODE_HOST[${tok[1]}]="${tok[3]}"
            NODE_PORT[${tok[1]}]="${tok[4]}"
            NODE_PRIO[${tok[1]}]="${tok[5]:-}"
            ;;
        client)
            [[ ${#tok[@]} -ge 3 ]] || { conf_err "linha $lineno: client <nome> <host> [super-peer] [port=P]"; return 1; }
            CLIENT_LIST+=("${tok[1]}")
            CLIENT_HOST[${tok[1]}]="${tok[2]}"
            CLIENT_SP[${tok[1]}]=""
            CLIENT_PORT[${tok[1]}]=""
            for opt in "${tok[@]:3}"; do
                case "$opt" in
                port=*) CLIENT_PORT[${tok[1]}]="${opt#port=}" ;;
                *) CLIENT_SP[${tok[1]}]="$opt" ;;
                esac
            done
            ;;
        *)
            conf_err "linha $lineno: diretiva desconhecida: ${tok[0]}"
            return 1
            ;;
        esac
    done <"$file"

    [[ ${#HOST_LIST[@]} -gt 0 ]] || { conf_err "nenhum host definido"; return 1; }
    conf_expand
}

# Completa IP e diretório de cada host e cria os Super Peers e clientes automáticos.
conf_expand() {
    local h n k=0 j=0 i ports p name target
    local -a explicit_nodes=("${NODE_LIST[@]}")
    local -a explicit_clients=("${CLIENT_LIST[@]}")
    local -A sp_of_host=()

    NODE_LIST=()
    CLIENT_LIST=()

    for h in "${HOST_LIST[@]}"; do
        target="${HOST_TARGET[$h]}"
        if [[ -z "${HOST_IP[$h]}" ]]; then
            if [[ "$target" == local ]]; then
                HOST_IP[$h]=127.0.0.1
            else
                HOST_IP[$h]="$(target_ip "$target")"
            fi
        fi
        [[ -n "${HOST_IP[$h]}" ]] ||
            { conf_err "host $h: não consegui descobrir o IP de '$target' (use ip=)"; return 1; }
        [[ -n "${HOST_DIR[$h]}" ]] || HOST_DIR[$h]="${CL_SET[remote_dir]}"
    done

    # Super Peers avulsos primeiro, na ordem do arquivo.
    for name in "${explicit_nodes[@]}"; do
        NODE_LIST+=("$name")
        sp_of_host[${NODE_HOST[$name]}]+="$name "
    done

    for h in "${HOST_LIST[@]}"; do
        if [[ -n "${HOST_SPPORTS[$h]}" ]]; then
            IFS=',' read -ra ports <<<"${HOST_SPPORTS[$h]}"
            n=${#ports[@]}
        else
            ports=()
            n="${HOST_SPS[$h]:-${CL_SET[sp_per_host]}}"
        fi
        for ((i = 0; i < n; i++)); do
            k=$((k + 1))
            name="SP$k"
            NODE_LIST+=("$name")
            NODE_HOST[$name]="$h"
            if [[ ${#ports[@]} -gt 0 ]]; then
                NODE_PORT[$name]="${ports[$i]}"
            else
                NODE_PORT[$name]=$((CL_SET[sp_port_base] + k))
            fi
            NODE_PRIO[$name]=""
            sp_of_host[$h]+="$name "
        done
    done

    # Prioridade sequencial (maior vence o Bully) onde não foi dada.
    k=0
    for name in "${NODE_LIST[@]}"; do
        k=$((k + 1))
        [[ -n "${NODE_PRIO[$name]}" ]] || NODE_PRIO[$name]=$k
    done

    # Clientes avulsos, depois os automáticos.
    for name in "${explicit_clients[@]}"; do
        CLIENT_LIST+=("$name")
    done
    for h in "${HOST_LIST[@]}"; do
        n="${HOST_CLIENTS[$h]:-${CL_SET[clients_per_host]}}"
        local -a mine
        read -ra mine <<<"${sp_of_host[$h]:-}"
        for ((i = 0; i < n; i++)); do
            j=$((j + 1))
            name="C$j"
            CLIENT_LIST+=("$name")
            CLIENT_HOST[$name]="$h"
            CLIENT_PORT[$name]=""
            if [[ ${#mine[@]} -gt 0 ]]; then
                CLIENT_SP[$name]="${mine[$((i % ${#mine[@]}))]}"
            else
                CLIENT_SP[$name]="${NODE_LIST[$(((j - 1) % ${#NODE_LIST[@]}))]:-}"
            fi
        done
    done

    # Porta e Super Peer de entrada que faltam nos clientes avulsos.
    j=0
    for name in "${CLIENT_LIST[@]}"; do
        j=$((j + 1))
        [[ -n "${CLIENT_PORT[$name]}" ]] || CLIENT_PORT[$name]=$((CL_SET[client_port_base] + j))
        [[ -n "${CLIENT_SP[$name]}" ]] || CLIENT_SP[$name]="${NODE_LIST[0]:-}"
    done
}

conf_validate() {
    local name h port rc=0
    local -A seen_name=() seen_port=()

    if [[ ${#NODE_LIST[@]} -eq 0 ]]; then
        conf_err "nenhum Super Peer definido"
        return 1
    fi
    if ((${#NODE_LIST[@]} > ROSTER_MAX)); then
        conf_err "${#NODE_LIST[@]} Super Peers; o parser de roster aceita no máximo $ROSTER_MAX"
        rc=1
    fi
    for name in "${NODE_LIST[@]}"; do
        h="${NODE_HOST[$name]}"
        port="${NODE_PORT[$name]}"
        [[ -n "${HOST_TARGET[$h]:-}" ]] || { conf_err "$name: host desconhecido: $h"; rc=1; }
        [[ "$port" =~ ^[0-9]+$ ]] && ((port >= 1 && port <= 65535)) ||
            { conf_err "$name: porta inválida: $port"; rc=1; }
        [[ -z "${seen_name[$name]:-}" ]] || { conf_err "nome repetido: $name"; rc=1; }
        seen_name[$name]=1
        # node_roster_load escolhe a linha do roster só pela porta (src/common/config.c).
        if [[ -n "${seen_port[$port]:-}" ]]; then
            conf_err "porta $port repetida (${seen_port[$port]} e $name): o roster identifica o nó pela porta"
            rc=1
        fi
        seen_port[$port]="$name"
    done
    for name in "${CLIENT_LIST[@]}"; do
        h="${CLIENT_HOST[$name]}"
        [[ -n "${HOST_TARGET[$h]:-}" ]] || { conf_err "cliente $name: host desconhecido: $h"; rc=1; }
        [[ -z "${seen_name[$name]:-}" ]] || { conf_err "nome repetido: $name"; rc=1; }
        seen_name[$name]=1
        [[ -n "${NODE_HOST[${CLIENT_SP[$name]}]:-}" ]] ||
            { conf_err "cliente $name: Super Peer de entrada desconhecido: ${CLIENT_SP[$name]}"; rc=1; }
    done
    return "$rc"
}

# ip:porta de um Super Peer.
node_endpoint() {
    local name="$1"
    printf '%s:%s' "${HOST_IP[${NODE_HOST[$name]}]}" "${NODE_PORT[$name]}"
}

node_ip() { printf '%s' "${HOST_IP[${NODE_HOST[$1]}]}"; }

# Nome do Super Peer que tem esse ip:porta (vazio se nenhum).
node_by_endpoint() {
    local ep="$1" name
    for name in "${NODE_LIST[@]}"; do
        [[ "$(node_endpoint "$name")" == "$ep" ]] && { printf '%s' "$name"; return 0; }
    done
    return 1
}

# Nó de maior prioridade: é quem vence o Bully quando todos estão vivos.
node_highest() {
    local name best="" bestp=-1
    for name in "${NODE_LIST[@]}"; do
        if ((NODE_PRIO[$name] > bestp)); then
            best="$name"
            bestp="${NODE_PRIO[$name]}"
        fi
    done
    printf '%s' "$best"
}

# Nome lógico do host de um nó ou cliente.
proc_host() {
    local name="$1"
    if [[ -n "${NODE_HOST[$name]:-}" ]]; then
        printf '%s' "${NODE_HOST[$name]}"
    else
        printf '%s' "${CLIENT_HOST[$name]:-}"
    fi
}

procs_on_host() {
    local h="$1" name
    for name in "${NODE_LIST[@]}"; do
        [[ "${NODE_HOST[$name]}" == "$h" ]] && printf '%s\n' "$name"
    done
    for name in "${CLIENT_LIST[@]}"; do
        [[ "${CLIENT_HOST[$name]}" == "$h" ]] && printf '%s\n' "$name"
    done
}

clients_on_host() {
    local h="$1" name
    for name in "${CLIENT_LIST[@]}"; do
        [[ "${CLIENT_HOST[$name]}" == "$h" ]] && printf '%s\n' "$name"
    done
}

nodes_on_host() {
    local h="$1" name
    for name in "${NODE_LIST[@]}"; do
        [[ "${NODE_HOST[$name]}" == "$h" ]] && printf '%s\n' "$name"
    done
}

# Escreve o roster no formato de node_roster_load: nome,tipo,ip,porta,id,prioridade.
conf_write_roster() {
    local out="$1" name k=0

    {
        echo "# Gerado por tests/cluster/cluster.sh a partir do cluster.conf. Não edite."
        for name in "${NODE_LIST[@]}"; do
            k=$((k + 1))
            printf '%s,superpeer,%s,%s,%08x,%s\n' "$name" "$(node_ip "$name")" "${NODE_PORT[$name]}" \
                "$k" "${NODE_PRIO[$name]}"
        done
    } >"$out"
}

# .conf de type=peer: o Super Peer de entrada vem primeiro no bootstrap, seguido de
# outros (até CONFIG_BOOTSTRAP_MAX = 8) para o cliente continuar se o primeiro cair.
conf_write_client_conf() {
    local client="$1" out="$2" entry boot name n=1

    entry="${CLIENT_SP[$client]}"
    boot="$(node_endpoint "$entry")"
    for name in "${NODE_LIST[@]}"; do
        ((n >= 8)) && break
        [[ "$name" == "$entry" ]] && continue
        boot+=",$(node_endpoint "$name")"
        n=$((n + 1))
    done
    {
        echo "# Gerado por tests/cluster/cluster.sh. Não edite."
        echo "ip=${HOST_IP[${CLIENT_HOST[$client]}]}"
        echo "port=${CLIENT_PORT[$client]}"
        echo "type=peer"
        echo "bootstrap=$boot"
    } >"$out"
}
