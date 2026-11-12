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

# Retorna o NodeID anunciado na inicialização do Super Peer.
node_id_hex() {
    logical_log <"$(node_log "$1")" 2>/dev/null | sed -nE 's/.*NodeID: ([[:xdigit:]]{64}).*/\1/p' | head -n1 | tr 'A-F' 'a-f'
}

# Escolhe o Super Peer com o maior NodeID hexadecimal entre os nós informados.
highest_node_id() {
    local LC_ALL=C name id best_name="" best_id=""
    for name in "$@"; do
        id="$(node_id_hex "$name")"
        [[ "$id" =~ ^[0-9a-f]{64}$ ]] || return 1
        if [[ -z "$best_id" || "$id" > "$best_id" ]]; then
            best_name="$name"
            best_id="$id"
        fi
    done
    [[ -n "$best_name" ]] || return 1
    printf '%s' "$best_name"
}

# Sobe os Super Peers e espera o maior NodeID assumir e ser reconhecido pelos demais.
# Define COORD, COORD_HEX e a métrica de eleição.
boot_cluster() {
    local n first_ms ms

    cl up || die "cluster.sh up falhou"
    local to=$((30 + 2 * ${#NODE_LIST[@]}))

    for n in "${NODE_LIST[@]}"; do
        wait_log "$(node_log "$n")" '^NodeID: [[:xdigit:]]{64}$' 10 "$n anunciar o NodeID"
    done
    COORD="$(highest_node_id "${NODE_LIST[@]}")" || die "não consegui determinar o maior NodeID nos logs"
    COORD_HEX="$(node_id_hex "$COORD")"
    local coord_pattern="^C4 COORDINATOR NodeID=${COORD_HEX}([[:space:]]|$)"
    wait_log "$(node_log "$COORD")" "$coord_pattern" "$to" "$COORD (maior NodeID) anunciar-se coordenador"

    for n in "${NODE_LIST[@]}"; do
        [[ "$n" == "$COORD" ]] && continue
        wait_log "$(node_log "$n")" "^C4 COORDINATOR aceito NodeID=${COORD_HEX}([[:space:]]|$)" 30 "$n aceitar o coordenador $COORD"
    done
    for n in "${NODE_LIST[@]}"; do
        wait_log "$(node_log "$n")" '^Successor -> ' 30 "$n publicar o sucessor"
    done

    first_ms=""
    for n in "${NODE_LIST[@]}"; do
        ms="$(first_stamp_ms "$(node_log "$n")")"
        if [[ -z "$first_ms" ]] || ((ms < first_ms)); then first_ms="$ms"; fi
    done
    ms="$(stamp_ms "$(node_log "$COORD")" "$coord_pattern")"
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
