# Roteiro do CP6: uma fase por item da lista do professor, com os Super Peers e
# clientes espalhados pelas máquinas do cluster.conf.
#   Aluno 2: Chord, Gossip, Bully, SMR, 2PC, IST
#   Aluno 1: upload, download, chunks, compression, cache, replica

N=${#NODE_LIST[@]}
((N >= 3)) || die "o cenário final precisa de pelo menos 3 Super Peers (há $N)"
NAMES_ABC=(documento_a.pdf documento_b.pdf documento_c.pdf)
NAMES_ALL=(documento_a.pdf documento_b.pdf documento_c.pdf documento_d.pdf documento_e.pdf)

phase "Preparo: arquivos de teste nas máquinas"
cl fixtures >/dev/null || die "fixtures falhou"
phase_done

phase "Bully e Chord: boot de $N Super Peers em ${#HOST_LIST[@]} máquina(s)"
boot_cluster
phase_done "coordenador $COORD"

phase "Clientes persistentes: ${#CLIENT_LIST[@]} abertos"
clients_start || die "clientes não subiram"
C1="${CLIENT_LIST[0]}"
E1="${CLIENT_SP[$C1]}"
C_OTHER="$(other_host_client "$C1")"
phase_done "$C1 entra por $E1; $C_OTHER em outra máquina"

# O nó que cai não pode ser o coordenador nem a porta de entrada de C1.
VICTIM=""
for n in "${NODE_LIST[@]}"; do
    [[ "$n" != "$COORD" && "$n" != "$E1" ]] && { VICTIM="$n"; break; }
done
[[ -n "$VICTIM" ]] || die "sem Super Peer disponível para derrubar"

phase "SMR e 2PC: uploads de A, B e C por $C1 (YES esperados: $((N - 1)))"
for l in a b c; do
    cdo "$C1" upload "$(client_file "$C1" "documento_${l}.pdf")"
done
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
assert_round "$(node_log "$COORD")" documento_a.pdf $((N - 1)) 1
assert_round "$(node_log "$COORD")" documento_b.pdf $((N - 1)) 2
assert_round "$(node_log "$COORD")" documento_c.pdf $((N - 1)) 3
for n in "${NODE_LIST[@]}"; do
    [[ "$n" == "$COORD" ]] && continue
    assert_log_sequence "$(node_log "$n")" "${NAMES_ABC[@]}"
done
for n in "${NODE_LIST[@]}"; do
    cl_assert_state "$n" 3 3 "${NAMES_ABC[@]}"
done
phase_done "mesmo log e mesmo estado nos $N nós"

phase "Chord: lookup pelo ObjectID de A"
OID="$(client_output "$C1" 1 | sed -n 's/^ObjectID: //p' | head -n1)"
[[ "${#OID}" -eq 64 ]] || die "não achei o ObjectID de A na saída do upload de $C1"
cdo "$C1" lookup "$OID"
client_log_has "$C1" "Lookup path:" || die "lookup sem 'Lookup path:'"
OWNER_EP="$(sed -E 's/^[0-9:.]+ //' "$(client_log "$C1")" | sed -n 's/^Owner: //p' | tail -n1)"
OWNER="$(node_by_endpoint "$OWNER_EP")" || die "dono $OWNER_EP não é um Super Peer do cluster"
say "ObjectID $OID pertence a $OWNER ($OWNER_EP)"
phase_done "dono: $OWNER"

phase "Gossip e detecção: kill -9 de $VICTIM"
KILL_MS="$(now_ms)"
node_kill "$VICTIM"
wait_log "$(node_log "$COORD")" "^(SUSPECT|FAILED) .*$(node_endpoint "$VICTIM")\$" 40 "$COORD marcar $VICTIM como SUSPECT ou FAILED"
SUSPECT_MS="$(stamp_ms "$(node_log "$COORD")" "^SUSPECT .*$(node_endpoint "$VICTIM")\$")"
[[ -n "$SUSPECT_MS" ]] && metric "detecção (kill até SUSPECT no coordenador)" "$(fmt_s $((SUSPECT_MS - KILL_MS)))" "~15 s (heartbeat 5 s, timeout 15 s)"
phase_done

phase "2PC com um participante fora: uploads de D e E (YES esperados: $((N - 2)))"
for l in d e; do
    cdo "$C1" upload "$(client_file "$C1" "documento_${l}.pdf")"
done
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
assert_round "$(node_log "$COORD")" documento_d.pdf $((N - 2)) 4
assert_round "$(node_log "$COORD")" documento_e.pdf $((N - 2)) 5
assert_name_absent "$(node_log "$VICTIM")" documento_d.pdf
assert_name_absent "$(node_log "$VICTIM")" documento_e.pdf
for n in "${NODE_LIST[@]}"; do
    [[ "$n" == "$VICTIM" ]] && continue
    cl_assert_state "$n" 5 5 "${NAMES_ALL[@]}"
done
phase_done

phase "IST: $VICTIM volta e recebe A até E"
node_start "$VICTIM" >/dev/null || die "não consegui subir $VICTIM de novo"
wait_log "$(node_log "$VICTIM")" '^version 0 -> 5$' 60 "IST de $VICTIM anunciar version 0 -> 5"
assert_lines_in_order "$(node_log "$VICTIM")" \
    "IST" "version 0 -> 5" "STATE_TRANSFER" \
    "LOG #1" "STORE documento_a.pdf" "LOG #2" "STORE documento_b.pdf" \
    "LOG #3" "STORE documento_c.pdf" "LOG #4" "STORE documento_d.pdf" \
    "LOG #5" "STORE documento_e.pdf"
IST_START="$(stamp_ms "$(node_log "$VICTIM")" '^IST$')"
IST_END="$(stamp_ms "$(node_log "$VICTIM")" '^STORE documento_e.pdf$')"
[[ -n "$IST_START" && -n "$IST_END" ]] && metric "IST (IST até STORE do último log)" "$(fmt_s $((IST_END - IST_START)))" "< 2 s (RNF-008)"
cl_assert_state "$VICTIM" 5 5 "${NAMES_ALL[@]}"
phase_done

phase "Cache LFU em $E1: duas consultas de A"
cdo "$C1" lookup documento_a.pdf
cdo "$C1" lookup documento_a.pdf
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
assert_lines_in_order "$(node_log "$E1")" "cache miss" "LFU frequency 1" "cache hit" "LFU frequency 2"
for n in "${NODE_LIST[@]}"; do
    [[ "$n" == "$E1" ]] && continue
    assert_exact_absent "$(node_log "$n")" "cache hit"
done
phase_done "miss e frequência 1, depois hit e frequência 2"

phase "Download: A por $C1 e por $C_OTHER (outra máquina), comparando o sha256"
download_and_check "$C1" documento_a.pdf a-c1.out documento_a.pdf
download_and_check "$C_OTHER" documento_a.pdf a-other.out documento_a.pdf
client_log_has "$C_OTHER" "SHA-256 verified" || die "download sem 'SHA-256 verified'"
phase_done

phase "Chunks e compressão: arquivo de 9 MiB por $C_OTHER"
cdo "$C_OTHER" upload "$(client_file "$C_OTHER" documento_big.pdf)"
client_log_has "$C_OTHER" "Chunks: 3" || die "upload de 9 MiB sem 'Chunks: 3'"
client_log_has "$C_OTHER" "Compression: LZ4" || die "upload sem 'Compression: LZ4'"
download_and_check "$C1" documento_big.pdf big-c1.out documento_big.pdf
phase_done "3 chunks de 4 MiB, sha256 idêntico"

phase "Réplica: derruba $OWNER (dono de A) e baixa A de novo"
REPL_CLIENT=""
for c in "${CLIENT_LIST[@]}"; do
    [[ "${CLIENT_SP[$c]}" != "$OWNER" ]] && { REPL_CLIENT="$c"; break; }
done
[[ -n "$REPL_CLIENT" ]] || die "todo cliente entra por $OWNER"
node_kill "$OWNER"
if [[ "$OWNER" == "$COORD" ]]; then
    say "$OWNER era o coordenador: esperando a nova eleição"
    ALIVE=()
    for n in "${NODE_LIST[@]}"; do
        [[ "$n" != "$OWNER" ]] && ALIVE+=("$n")
    done
    KILL_MS="$(now_ms)"
    wait_any_log '^highest NodeID wins$' 90 "algum Super Peer vivo vencer a nova eleição" "${ALIVE[@]}"
    metric "reeleição (kill do coordenador até a vitória)" "$(fmt_s $(($(now_ms) - KILL_MS)))" "detecção ~15 s + eleição < 5 s"
fi
sleep 3
download_and_check "$REPL_CLIENT" documento_a.pdf a-replica.out documento_a.pdf
phase_done "A baixado por $REPL_CLIENT sem $OWNER"
