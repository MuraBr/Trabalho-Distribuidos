# Encaixe para o conjunto de testes que o professor vai fornecer no CP6.
# Quando ele chegar, reproduza cada teste com as ferramentas já prontas:
#   boot_cluster                      sobe os Super Peers e espera a eleição
#   clients_start C1 C2               abre clientes persistentes
#   cdo C1 upload <arquivo>           comando de cliente; falha o cenário se rc != 0
#   node_kill SP2 / node_start SP2    injeção de falha e volta
#   wait_log <log> <regex> <seg> <o quê>   espera uma linha no log espelhado
#   assert_round, assert_log_sequence, cl_assert_state ...  (ver lib/asserts.sh)

phase "Boot"
boot_cluster
phase_done "coordenador $COORD"

die "scenarios/professor.sh ainda não tem os testes do professor"
