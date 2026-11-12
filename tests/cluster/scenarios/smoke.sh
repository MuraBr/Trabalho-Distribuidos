# Cenário curto: sobe tudo, um upload, um download em outra máquina, derruba.
# Serve para validar a rede, o ssh e o roster antes do cenário final.

phase "Preparo: arquivos de teste nas máquinas"
cl fixtures >/dev/null || die "fixtures falhou"
phase_done

phase "Boot: eleição e anel Chord em ${#NODE_LIST[@]} Super Peers"
boot_cluster
phase_done "coordenador $COORD"

phase "Clientes persistentes: um por máquina"
mapfile -t ACTIVE < <(first_clients)
clients_start "${ACTIVE[@]}" || die "clientes não subiram"
UP_CLIENT="${ACTIVE[0]}"
DOWN_CLIENT="${ACTIVE[-1]}"
phase_done "${ACTIVE[*]}"

phase "Upload por $UP_CLIENT e download por $DOWN_CLIENT"
cdo "$UP_CLIENT" upload "$(client_file "$UP_CLIENT" documento_a.pdf)"
client_log_has "$UP_CLIENT" "Compression: LZ4" || die "upload sem 'Compression: LZ4'"
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
download_and_check "$DOWN_CLIENT" documento_a.pdf smoke-a.out documento_a.pdf
client_log_has "$DOWN_CLIENT" "SHA-256 verified" || die "download sem 'SHA-256 verified'"
phase_done

phase "Consistência: LogIndex 1 em todos os Super Peers"
for n in "${NODE_LIST[@]}"; do
    cl_assert_state "$n" 1 1 documento_a.pdf
done
phase_done
