#!/usr/bin/env bash
# Passos reaproveitados pelos cenários.

cl() { bash "$CLUSTER_DIR/cluster.sh" "$@"; }

# Primeiro cliente de cada host (um por máquina), para cenários que não precisam de todos.
first_clients() {
    local h
    for h in "${HOST_LIST[@]}"; do
        clients_on_host "$h" | head -n1
    done
}

# Sobe os Super Peers e espera: eleição do coordenador esperado, COORDINATOR em todos
# e sucessor publicado em todos. Define COORD, COORD_HEX e a métrica de eleição.
boot_cluster() {
    local n first_ms ms
    local -a others=()

    cl up || die "cluster.sh up falhou"
    COORD="$(node_highest)"
    local to=$((30 + 2 * ${#NODE_LIST[@]}))

    wait_log "$(node_log "$COORD")" '^highest NodeID wins$' "$to" "$COORD (maior prioridade) vencer a eleição"
    wait_log "$(node_log "$COORD")" '^COORDINATOR ' 5 "$COORD anunciar o próprio NodeID"
    COORD_HEX="$(logical_log <"$(node_log "$COORD")" | awk '/^highest NodeID wins$/{getline; if ($1=="COORDINATOR"){print $2; exit}}')"
    [[ -n "$COORD_HEX" ]] || die "$COORD não imprimiu COORDINATOR depois de highest NodeID wins"

    for n in "${NODE_LIST[@]}"; do
        [[ "$n" == "$COORD" ]] && continue
        others+=("$n")
        wait_log "$(node_log "$n")" "^COORDINATOR ${COORD_HEX}\$" 30 "$n registrar o coordenador $COORD"
    done
    for n in "${NODE_LIST[@]}"; do
        wait_log "$(node_log "$n")" '^Successor -> ' 30 "$n publicar o sucessor"
    done

    first_ms=""
    for n in "${NODE_LIST[@]}"; do
        ms="$(first_stamp_ms "$(node_log "$n")")"
        if [[ -z "$first_ms" ]] || ((ms < first_ms)); then first_ms="$ms"; fi
    done
    ms="$(stamp_ms "$(node_log "$COORD")" '^highest NodeID wins$')"
    metric "eleição no boot (primeiro nó no ar até o coordenador vencer)" "$(fmt_s $((ms - first_ms)))" "< 5 s (RNF-010)"
    ok "coordenador $COORD ($COORD_HEX) e anel com sucessor em ${#NODE_LIST[@]} nós"
}

# Diretório de saída do cliente (absoluto no host dele).
client_out() { printf '%s/%s' "$(client_dir "$1")" "$2"; }

# Primeiro cliente de um host diferente do de $1 (se só há um host, o segundo cliente).
other_host_client() {
    local from="$1" c
    for c in "${CLIENT_LIST[@]}"; do
        [[ "${CLIENT_HOST[$c]}" != "${CLIENT_HOST[$from]}" ]] && { printf '%s' "$c"; return 0; }
    done
    for c in "${CLIENT_LIST[@]}"; do
        [[ "$c" != "$from" ]] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

# download <cliente> <nome> <saida-relativa>; confere o sha256 contra o arquivo original.
download_and_check() {
    local c="$1" name="$2" outname="$3" src="$4" h want got
    h="${CLIENT_HOST[$c]}"
    cdo "$c" download "$name" "$(client_out "$c" "$outname")"
    want="$(remote_sha "$h" "$(client_file "$c" "$src")")"
    got="$(remote_sha "$h" "$(client_out "$c" "$outname")")"
    [[ -n "$want" && "$want" == "$got" ]] || die "download de $name por $c difere do original (sha256 $got, esperado $want)"
    say "download de $name por $c idêntico ao original ($want)"
}
