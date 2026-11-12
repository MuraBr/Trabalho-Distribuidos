#!/usr/bin/env bash
# Clientes persistentes. Cada um é um `bin/client --interactive` rodando na máquina
# do host, com a stdin ligada a run/<nome>/in.fifo. Fica aberto até receber `quit`.
# Cada comando termina com "[done N rc=R]" no log; é isso que client_wait espera.

client_log() { printf '%s/%s.log' "$LOGS" "$1"; }

client_sent_file() { printf '%s/%s.sent' "$STATE" "$1"; }

client_sent() { cat "$(client_sent_file "$1")" 2>/dev/null || echo 0; }

# Caminho absoluto de um arquivo de teste (run/files/...) no host do cliente.
client_file() {
    local name="$1" f="$2"
    printf '%s/run/files/%s' "$(host_abs "${CLIENT_HOST[$name]}")" "$f"
}

# Diretório de trabalho do cliente no host (onde ficam o storage e as saídas).
client_dir() { printf '%s/run/%s' "$(host_abs "${CLIENT_HOST[$1]}")" "$1"; }

# Mostra as linhas de um comando de cliente entre o [done N-1] e o [done N].
client_output() {
    local name="$1" n="$2" file
    file="$(client_log "$name")"
    sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} //' "$file" |
        awk -v prev="$((n - 1))" -v cur="$n" '
            BEGIN { on = 0 }
            $0 ~ "^\\[ready\\]$" { if (prev == 0) { on = 1 }; next }
            $0 ~ "^\\[done " prev " rc=" { on = 1; next }
            on { print }
            $0 ~ "^\\[done " cur " rc=" { exit }'
}

# Sobe os clientes dados (padrão: todos) e espera o [ready] de cada um.
clients_start() {
    local -a names=("$@")
    local name h abs
    [[ ${#names[@]} -gt 0 ]] || names=("${CLIENT_LIST[@]}")
    local -A by_host=()
    for name in "${names[@]}"; do
        by_host[${CLIENT_HOST[$name]}]+="$name "
    done
    for h in "${!by_host[@]}"; do
        mirror_start "$h" || return 1
    done
    local -a pids=() hs=()
    for h in "${!by_host[@]}"; do
        abs="$(host_abs "$h")" || return 1
        (
            for name in ${by_host[$h]}; do
                printf '%q ' bash "$abs/run/remote.sh" "$abs/run" start-client "$name" -- \
                    "$abs/bin/client" --interactive --config "$abs/run/$name/peer.conf"
                printf '\n'
            done | rexec_t 180 "$h" "while IFS= read -r l; do eval \"\$l\" || exit 1; done"
        ) >/dev/null &
        pids+=("$!")
        hs+=("$h")
    done
    local i rc=0
    for i in "${!pids[@]}"; do
        wait "${pids[$i]}" || { echo "falha ao subir clientes em ${hs[$i]}" >&2; rc=1; }
    done
    [[ $rc -eq 0 ]] || return 1
    for name in "${names[@]}"; do
        echo 0 >"$(client_sent_file "$name")"
    done
    for name in "${names[@]}"; do
        wait_line "$(client_log "$name")" '^\[ready\]$' 20 || {
            echo "cliente $name não ficou pronto" >&2
            return 1
        }
    done
    say "${#names[@]} cliente(s) abertos"
}

# Espera uma linha casando com o padrão (extended regex, depois do carimbo).
wait_line() {
    local file="$1" pattern="$2" timeout="$3" end
    end=$((SECONDS + timeout))
    while ((SECONDS < end)); do
        if [[ -e "$file" ]] && sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} //' "$file" | grep -Eq "$pattern"; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# client_send <nome> <linha...>: escreve o comando e imprime o número dele na sessão.
client_send() {
    local name="$1" n
    shift
    rrun "${CLIENT_HOST[$name]}" send "$name" "$*" || return 1
    n=$(($(client_sent "$name") + 1))
    echo "$n" >"$(client_sent_file "$name")"
    printf '%s\n' "$n"
}

# client_send_many: lê "nome linha..." da stdin e manda tudo, uma conexão por host.
client_send_many() {
    local line name h
    local -A by_host=()
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        name="${line%% *}"
        h="${CLIENT_HOST[$name]}"
        by_host[$h]+="$line"$'\n'
        echo $(($(client_sent "$name") + 1)) >"$(client_sent_file "$name")"
    done
    for h in "${!by_host[@]}"; do
        printf '%s' "${by_host[$h]}" | rrun "$h" send-lines || return 1
    done
}

# client_wait <nome> <N> [timeout]: espera "[done N rc=R]" e devolve R.
client_wait() {
    local name="$1" n="$2" timeout="${3:-60}" file rc end
    file="$(client_log "$name")"
    end=$((SECONDS + timeout))
    while ((SECONDS < end)); do
        rc="$(sed -nE "s/^[0-9:.]+ \[done $n rc=(-?[0-9]+)\]\$/\1/p" "$file" 2>/dev/null | head -n1)"
        if [[ -n "$rc" ]]; then
            ((rc < 0)) && rc=255
            return "$rc"
        fi
        sleep 0.2
    done
    echo "tempo esgotado (${timeout}s) esperando o comando $n do cliente $name" >&2
    return 124
}

# client_do <nome> <linha...>: envia, espera e devolve o rc do comando.
client_do() {
    local name="$1" n
    shift
    n="$(client_send "$name" "$@")" || return 1
    client_wait "$name" "$n" "${CLIENT_TIMEOUT:-60}"
}

# client_batch <nome> <arquivo>: manda um arquivo de comandos e espera o último.
client_batch() {
    local name="$1" file="$2" line trimmed n=0 last
    [[ -f "$file" ]] || { echo "arquivo não encontrado: $file" >&2; return 1; }
    local payload=""
    while IFS= read -r line || [[ -n "$line" ]]; do
        trimmed="${line#"${line%%[![:space:]]*}"}"
        [[ -z "$trimmed" || "$trimmed" == \#* ]] && continue
        [[ "$trimmed" == quit || "$trimmed" == exit ]] && break
        payload+="$name $trimmed"$'\n'
        n=$((n + 1))
    done <"$file"
    ((n > 0)) || return 0
    last=$(($(client_sent "$name") + n))
    printf '%s' "$payload" | client_send_many || return 1
    client_wait "$name" "$last" "${CLIENT_TIMEOUT:-120}"
}

# client_stop: pede o fim da sessão (quit); se não fechar em 5 s, SIGTERM/SIGKILL.
client_stop() {
    local name="$1"
    rrun "${CLIENT_HOST[$name]}" send "$name" quit 2>/dev/null || true
    if ! wait_line "$(client_log "$name")" '^\[bye\]$' 5; then
        rrun "${CLIENT_HOST[$name]}" stop "$name" 2>/dev/null || true
    fi
}

clients_stop() {
    local -a names=("$@")
    local name
    [[ ${#names[@]} -gt 0 ]] || names=("${CLIENT_LIST[@]}")
    local -a pids=()
    for name in "${names[@]}"; do
        ( client_stop "$name" ) &
        pids+=("$!")
    done
    local p
    for p in "${pids[@]}"; do wait "$p" || true; done
}

# Digita direto na sessão do cliente (Ctrl-D sai).
client_attach() {
    local name="$1" h abs
    h="${CLIENT_HOST[$name]}"
    abs="$(host_abs "$h")"
    echo "conectado a $name em $h. Digite comandos; Ctrl-D sai (o cliente continua aberto)."
    ( rexec_stream "$h" "cat > '$abs/run/$name/in.fifo'" )
}

clients_status() {
    local h name state pid life
    printf '%-8s %-6s %-6s %-6s %s\n' NOME HOST ESTADO VIDA SUPERPEER
    for h in "${HOST_LIST[@]}"; do
        local -a names
        mapfile -t names < <(clients_on_host "$h")
        [[ ${#names[@]} -gt 0 ]] || continue
        while read -r name state pid life; do
            printf '%-8s %-6s %-6s %-6s %s\n' "$name" "$h" "$state" "$life" "${CLIENT_SP[$name]}"
        done < <(rrun "$h" alive "${names[@]}" 2>/dev/null || true)
    done
}
