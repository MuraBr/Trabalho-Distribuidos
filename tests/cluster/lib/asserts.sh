#!/usr/bin/env bash
# Conferências dos cenários. As de log (wait_log, assert_round, assert_log_sequence,
# assert_lines_in_order, assert_exact_absent, assert_name_absent) são as do harness de
# fluxo (tests/flow/lib.sh) e leem os logs espelhados em tests/cluster/logs.

# shellcheck source=../../flow/lib.sh
source "$CLUSTER_DIR/../flow/lib.sh"

# O flow/lib.sh instala um trap que mata PIDS locais; aqui quem encerra é o cluster.
trap - EXIT INT TERM

FLOW_LOG="$LOGS"
FLOW_OUT="$OUT"

REPORT_FILE="$OUT/relatorio.md"
REPORT_ROWS=()
REPORT_METRICS=()
CURRENT_PHASE=""

phase() {
    CURRENT_PHASE="$*"
    section "$*"
    say "fase: $*"
}

# Registra o resultado da fase atual no relatório (chamada no fim de cada fase).
phase_done() {
    REPORT_ROWS+=("| $CURRENT_PHASE | PASS | ${1:-} |")
    ok "$CURRENT_PHASE"
}

metric() {
    REPORT_METRICS+=("| $1 | $2 | $3 |")
    say "métrica: $1 = $2 (alvo: $3)"
}

# O die do flow imprime e sai; este registra a fase antes.
die() {
    say "FALHA: $*"
    fail "$*"
    REPORT_ROWS+=("| ${CURRENT_PHASE:-?} | FAIL | $* |")
    summary
    exit 1
}

write_report() {
    {
        echo "# Relatório do cenário — $(date '+%d/%m/%Y %H:%M:%S')"
        echo
        echo "Hosts: ${#HOST_LIST[@]}, Super Peers: ${#NODE_LIST[@]}, clientes: ${#CLIENT_LIST[@]}."
        echo
        echo "| Fase | Resultado | Detalhe |"
        echo "| --- | --- | --- |"
        printf '%s\n' "${REPORT_ROWS[@]}"
        if [[ ${#REPORT_METRICS[@]} -gt 0 ]]; then
            echo
            echo "| Métrica | Medido | Alvo |"
            echo "| --- | --- | --- |"
            printf '%s\n' "${REPORT_METRICS[@]}"
        fi
        echo
        echo "Logs de cada processo: \`tests/cluster/logs/\`."
    } >"$REPORT_FILE"
    say "relatório em $REPORT_FILE"
}

# --- tempo --------------------------------------------------------------

now_ms() {
    local t="${EPOCHREALTIME/,/.}" ms
    ms="${t#*.}"
    local s="${t%.*}"
    # milissegundos desde a meia-noite local, como os carimbos dos logs
    local hms hh mm ss
    printf -v hms '%(%H %M %S)T' "$s"
    read -r hh mm ss <<<"$hms"
    echo $(((10#$hh * 3600 + 10#$mm * 60 + 10#$ss) * 1000 + 10#${ms:0:3}))
}

# Carimbo (ms desde a meia-noite) da última linha do log que casa com o padrão.
stamp_ms() {
    local file="$1" pattern="$2"
    awk -v pat="$pattern" '
        {
            line = $0
            sub(/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} /, "", line)
            if (line ~ pat) { stamp = substr($0, 1, 12) }
        }
        END {
            if (stamp != "") {
                split(stamp, a, /[:.]/)
                print ((a[1] * 3600 + a[2] * 60 + a[3]) * 1000 + a[4])
            }
        }' "$file"
}

# Carimbo da primeira linha do log.
first_stamp_ms() {
    local file="$1"
    head -n1 "$file" | awk '{
        split(substr($0, 1, 12), a, /[:.]/)
        print ((a[1] * 3600 + a[2] * 60 + a[3]) * 1000 + a[4])
    }'
}

fmt_s() {
    local ms="$1"
    printf '%d.%03d s' $((ms / 1000)) $((ms % 1000))
}

count_lines() {
    local file="$1" pattern="$2"
    [[ -e "$file" ]] || { echo 0; return; }
    logical_log <"$file" | grep -Ec "$pattern" || true
}

# Espera o padrão aparecer mais vezes do que $min (contagem tirada antes da ação).
wait_count() {
    local file="$1" pattern="$2" min="$3" timeout="$4" desc="$5" end
    say "esperando até ${timeout}s em $(basename "$file"): $desc"
    end=$((SECONDS + timeout))
    while ((SECONDS < end)); do
        (($(count_lines "$file" "$pattern") > min)) && { say "apareceu: $desc"; return 0; }
        sleep 0.5
    done
    die "tempo esgotado (${timeout}s) em $(basename "$file"): $desc. Padrão: $pattern"
}

# Espera o padrão aparecer no log de qualquer um dos nós dados (ex.: eleição de um novo líder).
wait_any_log() {
    local pattern="$1" timeout="$2" desc="$3" n end
    shift 3
    say "esperando até ${timeout}s em ($*): $desc"
    end=$((SECONDS + timeout))
    while ((SECONDS < end)); do
        for n in "$@"; do
            if [[ -e "$(node_log "$n")" ]] && logical_log <"$(node_log "$n")" | grep -Eq "$pattern"; then
                say "apareceu em $n: $desc"
                return 0
            fi
        done
        sleep 0.5
    done
    die "tempo esgotado (${timeout}s): $desc. Padrão: $pattern"
}

# --- estado do Super Peer ----------------------------------------------

# Roda `bin/client --cmd state` no host do nó (cliente de um comando só) e imprime a saída.
node_state_text() {
    local name="$1" h abs
    h="${NODE_HOST[$name]}"
    abs="$(host_abs "$h")"
    REXEC_TIMEOUT=20 rexec "$h" "'$abs/bin/client' --cmd state --host '$(node_ip "$name")' --port ${NODE_PORT[$name]}"
}

# cl_assert_state <SP> <LogIndex> <Objects> <nomes...>
cl_assert_state() {
    local name="$1" index="$2" objects="$3" text n
    shift 3
    say "consultando state em $name: LogIndex $index, Objects: $objects, nomes: $*"
    text="$(node_state_text "$name")" || die "state de $name não respondeu"
    grep -qx "LogIndex $index" <<<"$text" || die "state de $name sem LogIndex $index"$'\n'"$text"
    grep -qx "Objects: $objects" <<<"$text" || die "state de $name sem Objects: $objects"$'\n'"$text"
    for n in "$@"; do
        grep -q "^${n} v" <<<"$text" || die "state de $name sem $n"
    done
    ok "state $name LogIndex $index, $objects objetos"
}

# --- coordenador e comandos de cliente ----------------------------------

# Nome do Super Peer que anunciou "highest NodeID wins" mais recentemente em algum log.
find_coordinator() {
    local name
    for name in "${NODE_LIST[@]}"; do
        if logical_log <"$(node_log "$name")" 2>/dev/null | grep -qx 'highest NodeID wins'; then
            printf '%s' "$name"
            return 0
        fi
    done
    return 1
}

# cdo <cliente> <linha...>: executa e falha o cenário se o rc não for 0.
cdo() {
    local name="$1" rc
    shift
    say "$name> $*"
    client_do "$name" "$@"
    rc=$?
    [[ $rc -eq 0 ]] || die "comando do cliente $name falhou (rc=$rc): $* — ver $(basename "$(client_log "$name")")"
}

client_log_has() {
    local name="$1" line="$2"
    logical_log <"$(client_log "$name")" | grep -qx -F "$line"
}

# sha256 de um arquivo no host do cliente.
remote_sha() {
    local h="$1" file="$2"
    rrun "$h" sha256 "$file"
}
