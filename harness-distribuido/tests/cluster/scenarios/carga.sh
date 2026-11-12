# Todos os clientes do cluster ao mesmo tempo: cada um envia o próprio arquivo
# e depois baixa o do cliente seguinte. Mede quantos terminam e em quanto tempo.
#   WAVE=<n>   quantos clientes agem por vez (padrão 10)

WAVE="${WAVE:-10}"
TOTAL=${#CLIENT_LIST[@]}

phase "Preparo"
cl fixtures >/dev/null || die "fixtures falhou"
phase_done

phase "Boot: $TOTAL clientes e ${#NODE_LIST[@]} Super Peers"
boot_cluster
clients_start || die "clientes não subiram"
# um arquivo único por cliente, criado na máquina dele
for h in "${HOST_LIST[@]}"; do
    abs="$(host_abs "$h")"
    script=""
    while read -r c; do
        script+="{ cat '$abs/run/files/documento_a.pdf'; echo '$c'; } > '$abs/run/files/carga_$c.pdf'; "
    done < <(clients_on_host "$h")
    rexec_t 60 "$h" "$script" || die "não criei os arquivos de carga em $h"
done
phase_done

run_wave() {
    local verb="$1" i end c n ok=0 bad=0
    local -A seq=()
    for ((i = 0; i < TOTAL; i += WAVE)); do
        end=$((i + WAVE))
        ((end > TOTAL)) && end=$TOTAL
        for ((j = i; j < end; j++)); do
            c="${CLIENT_LIST[$j]}"
            case "$verb" in
            upload) echo "$c upload $(client_file "$c" "carga_$c.pdf")" ;;
            download)
                nxt="${CLIENT_LIST[$(((j + 1) % TOTAL))]}"
                echo "$c download carga_$nxt.pdf $(client_out "$c" "carga.out")"
                ;;
            esac
        done | client_send_many
        for ((j = i; j < end; j++)); do
            c="${CLIENT_LIST[$j]}"
            n="$(client_sent "$c")"
            if client_wait "$c" "$n" 180; then ok=$((ok + 1)); else bad=$((bad + 1)); say "falhou: $c ($verb)"; fi
        done
    done
    RUN_OK=$ok
    RUN_BAD=$bad
}

phase "Upload simultâneo: $TOTAL clientes, $WAVE por vez"
T0=$(now_ms)
run_wave upload
metric "uploads concluídos ($TOTAL clientes)" "$RUN_OK ok, $RUN_BAD falhas em $(fmt_s $(($(now_ms) - T0)))" "todos"
((RUN_BAD == 0)) || die "$RUN_BAD upload(s) falharam"
phase_done "$RUN_OK uploads"

phase "Download simultâneo: cada cliente baixa o arquivo do seguinte"
T0=$(now_ms)
run_wave download
metric "downloads concluídos ($TOTAL clientes)" "$RUN_OK ok, $RUN_BAD falhas em $(fmt_s $(($(now_ms) - T0)))" "todos"
((RUN_BAD == 0)) || die "$RUN_BAD download(s) falharam"
phase_done "$RUN_OK downloads com SHA-256 verified"

phase "Consistência: mesmo LogIndex em todos os Super Peers"
mirror_sync "${NODE_LIST[@]}" || die "espelho de log atrasado"
for n in "${NODE_LIST[@]}"; do
    cl_assert_state "$n" "$TOTAL" "$TOTAL" "carga_${CLIENT_LIST[0]}.pdf"
done
phase_done
