#!/usr/bin/env bash
# Transporte: roda comando em um host (ssh, ou direto quando o alvo é "local"),
# copia arquivos e prepara as chaves ssh.

PROJECT_ROOT_REAL="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# ControlMaster: uma conexão ssh por host, reaproveitada por todos os comandos.
SSH_CTL_DIR="${XDG_RUNTIME_DIR:-/tmp}/btcluster-$(id -u)"
mkdir -p "$SSH_CTL_DIR" 2>/dev/null && chmod 700 "$SSH_CTL_DIR" 2>/dev/null

REXEC_TIMEOUT="${REXEC_TIMEOUT:-30}"
SSH_BEGIN_MARK="__BTC_BEGIN__"

host_is_local() { [[ "${HOST_TARGET[$1]}" == local ]]; }

# Diretório do projeto no host, como texto para uso entre aspas duplas
# numa shell remota ($HOME é expandido lá, não aqui).
host_dir_expr() {
    local d="${HOST_DIR[$1]}"
    if host_is_local "$1" && [[ "$d" == "${CL_SET[remote_dir]}" ]]; then
        # host local sem dir= próprio: usa este mesmo checkout
        d="$PROJECT_ROOT_REAL"
    fi
    printf '%s' "${d/#\~/\$HOME}"
}

_ssh_args() {
    local h="$1"
    # shellcheck disable=SC2206
    local -a opts=(${CL_SET[ssh_opts]})
    printf '%s\n' "${opts[@]}" -o ControlMaster=auto -o ControlPersist=120 \
        -o "ControlPath=$SSH_CTL_DIR/%C" -o LogLevel=ERROR
}

# rexec <host> <comando-shell...>: o comando é uma string interpretada na máquina do host.
rexec() {
    local h="$1"
    shift
    local script="$*"
    if host_is_local "$h"; then
        timeout "$REXEC_TIMEOUT" bash -c "$script"
        return
    fi
    local -a args
    local rc
    mapfile -t args < <(_ssh_args "$h")
    # .bashrc ou motd barulhentos da máquina remota escreveriam antes do nosso comando
    # e estragariam qualquer saída capturada; tudo até o marcador é descartado.
    timeout "$REXEC_TIMEOUT" ssh "${args[@]}" "${HOST_TARGET[$h]}" "echo $SSH_BEGIN_MARK; $script" |
        sed -u -n "/^$SSH_BEGIN_MARK\$/,\${/^$SSH_BEGIN_MARK\$/!p}"
    rc="${PIPESTATUS[0]}"
    return "$rc"
}

# Igual a rexec, com tempo limite próprio (build, rsync).
rexec_t() {
    local t="$1"
    shift
    REXEC_TIMEOUT="$t" rexec "$@"
}

# rexec lendo a stdin do chamador.
rexec_stdin() { rexec "$@"; }

# Comando ssh interativo/streaming (tail -F, cat > fifo): sem timeout.
rexec_stream() {
    local h="$1"
    shift
    local script="$*"
    if host_is_local "$h"; then
        exec bash -c "$script"
    fi
    local -a args
    mapfile -t args < <(_ssh_args "$h")
    exec ssh "${args[@]}" "${HOST_TARGET[$h]}" "$script"
}

# Envia o diretório local $1 para o diretório do host (conteúdo, não a pasta).
# Usa tar por ssh: não exige rsync nas máquinas.
push_tree() {
    local h="$1" src="$2" dest_expr="$3"
    shift 3
    local -a excl=()
    local e
    for e in "$@"; do excl+=("--exclude=$e"); done
    tar -C "$src" "${excl[@]}" -cf - . |
        REXEC_TIMEOUT=300 rexec "$h" "mkdir -p \"$dest_expr\" && tar -C \"$dest_expr\" -xf -"
}

# Código do projeto para o host. O que é gerado em cada máquina não vai.
sync_code() {
    local h="$1"
    if host_is_local "$h" && [[ "$(host_dir_expr "$h")" == "$PROJECT_ROOT_REAL" ]]; then
        return 0
    fi
    push_tree "$h" "$PROJECT_ROOT_REAL" "$(host_dir_expr "$h")" \
        ./.git ./.worktrees ./.idea ./bin ./obj ./data ./storage ./tests/data \
        ./docs ./.claude ./.cursor ./.github ./CLAUDE.md ./tests/flow/logs ./tests/flow/out ./tests/cluster/logs ./tests/cluster/out \
        ./tests/cluster/cluster.conf ./run
}

# Roda uma função para cada host em paralelo; a saída de cada um leva o prefixo [host].
par_hosts() {
    local fn="$1" h rc=0
    shift
    local -a pids=() hs=()
    for h in "${HOST_LIST[@]}"; do
        ( "$fn" "$h" "$@" 2>&1 | sed -u "s/^/[$h] /"; exit "${PIPESTATUS[0]}" ) &
        pids+=("$!")
        hs+=("$h")
    done
    local i
    for i in "${!pids[@]}"; do
        wait "${pids[$i]}" || { echo "falhou em ${hs[$i]}" >&2; rc=1; }
    done
    return "$rc"
}

# Fecha as conexões ssh persistentes.
ssh_close_all() {
    local h
    for h in "${HOST_LIST[@]}"; do
        host_is_local "$h" && continue
        local -a args
        mapfile -t args < <(_ssh_args "$h")
        ssh "${args[@]}" -O exit "${HOST_TARGET[$h]}" >/dev/null 2>&1 || true
    done
}

# --- chaves ssh ---------------------------------------------------------

# Quando o ssh_opts do conf tem BatchMode=yes, a cópia de chave precisa aceitar senha.
_opts_for_password() {
    local o="${CL_SET[ssh_opts]}"
    printf '%s' "${o//BatchMode=yes/BatchMode=no}"
}

# Garante que existe uma chave local e copia a pública para cada máquina que ainda
# pede senha. A senha vem de $SSHPASS ou de um prompt, é usada só durante esta
# chamada e nunca é gravada em arquivo.
copy_keys() {
    local per_host="${1:-}" h target pass="" key pub rc=0
    local -A done_target=()

    command -v ssh-copy-id >/dev/null || { echo "ssh-copy-id não encontrado" >&2; return 1; }
    command -v sshpass >/dev/null || { echo "sshpass não encontrado (instale para copiar a chave com senha)" >&2; return 1; }

    key=""
    for pub in "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_rsa" "$HOME/.ssh/id_ecdsa"; do
        [[ -f "$pub" && -f "$pub.pub" ]] && { key="$pub"; break; }
    done
    if [[ -z "$key" ]]; then
        echo "nenhuma chave ssh local; gerando $HOME/.ssh/id_ed25519 (sem senha)"
        mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh"
        ssh-keygen -q -t ed25519 -N '' -f "$HOME/.ssh/id_ed25519" || return 1
        key="$HOME/.ssh/id_ed25519"
    fi

    # shellcheck disable=SC2206
    local -a popts=($(_opts_for_password))
    for h in "${HOST_LIST[@]}"; do
        host_is_local "$h" && continue
        target="${HOST_TARGET[$h]}"
        [[ -z "${done_target[$target]:-}" ]] || continue
        done_target[$target]=1

        local -a bargs
        mapfile -t bargs < <(_ssh_args "$h")
        if ssh "${bargs[@]}" -o BatchMode=yes "$target" true 2>/dev/null; then
            echo "$h ($target): a chave já funciona"
            continue
        fi

        if [[ -n "${SSHPASS:-}" ]]; then
            pass="$SSHPASS"
        elif [[ -z "$pass" || -n "$per_host" ]]; then
            read -rsp "senha ssh de $target: " pass </dev/tty
            echo
        fi
        echo "$h ($target): copiando a chave"
        if SSHPASS="$pass" sshpass -e ssh-copy-id -i "$key.pub" "${popts[@]}" "$target"; then
            :
        else
            echo "$h ($target): falhou" >&2
            rc=1
            [[ -n "$per_host" ]] || pass=""
        fi
    done
    pass=""
    return "$rc"
}
