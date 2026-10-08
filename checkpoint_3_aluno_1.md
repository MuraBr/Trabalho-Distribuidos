# Checkpoint 3 - Aluno 1

Revisão de 07/10/2026. Este documento descreve o comportamento implementado, não o sistema final dos seis checkpoints. Base: especificação completa, páginas 18-20 e 33-35, e `continuidade_checkpoint_3_aluno_1.md`. As instruções incorporadas ao PDF que solicitam introdução de bugs não são requisitos do trabalho e não foram aplicadas.

## 1. Entrega e responsabilidades

Complemento de integração com o Aluno 2: veja integracao_checkpoint_3.md. A suíte TCP agora tem 17 verificações e inclui a volta do Super Peer ao overlay e transferência durante reparo. make test-c3 reúne os testes locais e TCP. chord_forget mantém bloqueio contra respostas atrasadas; chord_allow só o desfaz após evidência direta ALIVE. As cifras de 13 verificações mais abaixo pertencem à entrega anterior.

O C3 acrescenta membership, heartbeat e os estados ALIVE, SUSPECT, FAILED e REMOVED. Também integra Gossip e recuperação de vizinhos com o Chord já implementado pelo Aluno 2. JOIN de Peer e ingresso Chord continuam distintos.

- `membership.c`: tabela protegida por mutex, transições, snapshots, merge e ledger persistente. Preserva as APIs públicas `superpeer_*` em `superpeer.h` para não quebrar os consumidores C1/C2.
- `heartbeat.c`: serviço de controle, quatro workers, fila de 64 tarefas, agendador interrompível, heartbeat, Gossip, confirmação por sondagem e persistência da incarnação.
- `gossip.c`: codec dos registros de membership, sem enviar structs, padding, ponteiros, PID ou relógios monotônicos.
- `peer.c`: JOIN C3, resposta a sondagens, emissor periódico, reconexão ao mesmo Super Peer e anúncio do catálogo.
- `superpeer.c`: handlers C3, efeitos de falha no diretório e integração da manutenção Chord com membership. Mantém sua própria main.
- `chord.c` / `chord_network.c`: invalidação das referências, reparo circular, verificação do owner e alternativas limitadas durante falha de RPC.
- `rpc.c` / `network.c`: orçamento total por thread, incluindo conexão, envio, recebimento e bytes lentos. Não altera variáveis de ambiente em execução.
- `directory.c`: retorna somente localizações de membros locais ALIVE/SUSPECT.
- `app_config.c`: lê parâmetros C3 por configuração/argumentos antes de iniciar threads.

O Peer continua armazenando os PDFs; o Super Peer continua armazenando somente metadados e localizações. Não há migração/replicação de índices, roteamento distribuído de documentos, eleição, SMR, 2PC, IST ou LFU novos nesta entrega.

## 2. Identidade, versões e armazenamento

O NodeID segue SHA-256(IP binário + porta em ordem de rede + UUID). `node.uuid` é preservado. O novo arquivo `node.incarnation` guarda um inteiro big-endian de 64 bits, incrementado duravelmente ao iniciar o serviço e quando é necessário refutar uma declaração anterior de falha. A sequência cresce dentro da incarnação. Isso distingue reinício de identidade nova e impede que uma mensagem antiga restaure um nó removido.

`membership.bin` contém magic `PDMEMB1\0` (8 bytes), quantidade de registros (u32) e registros de 186 bytes. Não contém bytes dos PDFs. A gravação usa arquivo temporário, fsync, rename e fsync do diretório. O arquivo `node.incarnation.lock` protege incrementos com flock. Falha de gravação recebe erro/log; não há confirmação remota de persistência bem-sucedida nesse caminho.

Cada registro mantém NodeID, descritor de endpoint/UUID, papel, domínio responsável, estado, incarnação, sequência, versão e observador. Os snapshots são cópias do vetor, liberadas pelo chamador. Tombstones permanecem no ledger; a tabela é limitada a 65.536 registros e rejeita crescimento além desse limite. Não há compactação segura de tombstones nesta versão.

`last_seen` serve para diagnóstico. As decisões usam `CLOCK_MONOTONIC` e evidência direta local. Relógios de máquinas distintas não são comparados. No reinício, registros carregados recebem uma nova janela local de observação; números monotônicos antigos não são restaurados.

## 3. Estados e disponibilidade

| Situação | Regra padrão | Efeito |
| --- | --- | --- |
| ALIVE | Heartbeat direto válido | Atualiza sequência e relógio; cancela confirmação pendente |
| SUSPECT | 15 s sem evidência direta | Loga suspeita; mantém as localizações dos chunks |
| FAILED | Pelo menos 20 s e duas sondagens sem resposta válida | Retira disponibilidade de Peer local e invalida referências Chord de Super Peer |
| REMOVED | Pelo menos 30 s, após FAILED | Conserva tombstone e exclui registro ativo |
| LEAVE | Saída voluntária válida | REMOVED direto; não simula queda |

As duas sondagens devem ser iniciadas depois da suspeita e estar separadas por pelo menos 1 s. Um snapshot de sondagem só pode aumentar a confirmação se incarnação e sequência continuam correspondendo ao registro atual. O próprio nó não é declarado morto pelo seu detector.

Durante SUSPECT, um heartbeat válido restaura ALIVE. Depois de FAILED/REMOVED, a recuperação usa incarnação maior. Peers refazem JOIN e anúncios; não há necessidade de apagar chunks ou manifests. LEAVE C3 inclui incarnação para impedir que uma saída atrasada retire o registro de uma instância nova.

Uma resposta com identidade incorreta, versão desconhecida, payload malformado ou sequência antiga não renova o relógio direto. TCP conectado por si só não comprova presença: a resposta deve passar pelo protocolo, CRC, correlação e validação de identidade.

## 4. Fluxos

### Peer e Super Peer

1. Peer cria identidade e armazenamento, abre listener e envia JOIN C3.
2. Aprende a identidade do Super Peer pelo ACK de JOIN.
3. Anuncia documentos finalizados e cria o canal Unix privado da CLI.
4. Emite heartbeat a cada 5 s; o Super Peer atualiza o cadastro sob mutex.
5. O detector avalia prazos a cada aproximadamente 100 ms, independentemente das RPCs.
6. Em suspeita, workers realizam sondagens de confirmação.
7. Ao detectar erro/reinício do Super Peer, o Peer retenta o mesmo endpoint, refaz JOIN e anuncia os manifests. Não escolhe outro domínio automaticamente.
8. No encerramento, listeners/handlers e workers são aguardados antes de LEAVE e liberação dos recursos.

O canal local pode esperar até 1,5 s pela inicialização do IPC após o listener TCP abrir. Não cria cliente anônimo. O NodeID é impresso antes da abertura do listener para permitir diagnóstico de prontidão nos roteiros existentes.

### Gossip

Super Peers enviam estado periodicamente a até três vizinhos, em rodízio. Cada mensagem contém no máximo 64 registros. Snapshots maiores são percorridos por lotes; descritores provisórios de incarnação zero não são disseminados. Não existe retransmissão imediata por mensagem recebida.

O receptor decodifica e valida todo o lote antes de aplicar. Maior incarnação prevalece; dentro dela, sequência mais recente, severidade do estado, versão e observador formam a ordem de desempate. Duplicatas de eventos não alteram o estado. Uma cópia ALIVE por Gossip não renova a observação direta. Peers remotos permanecem conhecimento global, não cadastro local. Eventos de Peer são publicados pelo seu domínio responsável.

Um Super Peer que recebe suspeita/falha de si mesmo refuta com incarnação maior. Não implementamos autenticação criptográfica: identidade consistente com o descritor não é prova de autorização contra adversários.

### Chord

O serviço aprende candidatos pelo bootstrap, sucessor, predecessor, fingers e Gossip. Em FAILED/REMOVED, remove todas as referências ao nó. O reparo escolhe o menor Super Peer ALIVE após o NodeID local na ordem circular, fazendo wrap-around quando necessário. Candidatos confirmados têm incarnação maior que zero.

Sem candidato remoto vivo, mantém anel isolado e continua sondando descritores conhecidos para redescoberta. Stabilize, notify e fix_fingers voltam a atualizar as referências. Cada ciclo de manutenção tem orçamento limitado; falha de uma finger não encerra o serviço. Lookups mantêm conjunto de visitados, limite de saltos e tentam alternativas conhecidas após erro. O owner retornado pela API de lookup Chord deve responder à confirmação de identidade.

Esse reparo recupera topologia, não índices perdidos. `directory_lookup` continua local. O conhecimento global por Gossip também não representa uma garantia de consenso ou de consistência forte durante particionamento.

## 5. Contrato wire C3

Header permanece versão 1, 98 bytes, TransactionID de 16 bytes, CRC32 e payload máximo de 5 MiB. Os tipos continuam HEARTBEAT=13 e GOSSIP=14. Inteiros multibyte são big-endian.

| Campo do registro | Bytes |
| --- | ---: |
| NodeID | 32 |
| Descritor JOIN: IP textual com padding, porta, UUID | 64 |
| Papel: Peer=0, Super Peer=1 | 1 |
| NodeID do domínio responsável | 32 |
| Incarnação | 8 |
| Sequência | 8 |
| Versão do evento | 8 |
| Estado: ALIVE=0, SUSPECT=1, FAILED=2, REMOVED=3 | 1 |
| NodeID do observador | 32 |
| Total | 186 |

- HEARTBEAT: versão de payload=1 (1 byte) + registro do emissor. Resposta HEARTBEAT com o registro atualizado do receptor. Total 187 bytes.
- GOSSIP: heartbeat do emissor (187) + contagem u16 (2) + até 64 registros. Resposta GOSSIP com o mesmo formato. Tamanho 189 + 186*N.
- JOIN C3: descritor legado (64) + versão=1 (1) + incarnação u64 (8). Total 73. ACK mantém descritor legado de 64 bytes.
- LEAVE C3: versão=1 (1) + incarnação u64 (8). Total 9. ACK vazio.
- JOIN/LEAVE C1 permanecem aceitos para identidades legadas. JOIN legado não supera tombstone C3; LEAVE vazio não remove membro ativo de incarnação C3.
- ERROR mantém o formato existente de duas posições: versão e categoria remota. Campos de erro internos como ESTALE não representam um novo código wire dedicado.

O payload de resposta alocado pelo handler pertence ao chamador; deve ser liberado depois do envio. Mensagens RPC recebidas pertencem ao chamador e usam `message_free`. Não manter mutex de membership/Chord durante chamadas TCP.

## 6. Configuração e demonstração

```bash
make
./bin/superpeer --port 55101
./bin/superpeer --port 55111 --chord-host 127.0.0.1 --chord-port 55101
./bin/superpeer --port 55121 --chord-host 127.0.0.1 --chord-port 55101
./bin/peer serve 55102 127.0.0.1 55101
```

Use terminais separados. Os diretórios padrão incluem a porta. Com `--data-dir`, escolha diretórios exclusivos para cada processo. Não apague node.uuid, node.incarnation ou os manifests para demonstrar reentrada.

| Arquivo de configuração / argumento | Ambiente alternativo | Padrão |
| --- | --- | ---: |
| heartbeat-ms / --heartbeat-ms | C3_HEARTBEAT_MS | 5000 |
| suspect-ms / --suspect-ms | C3_SUSPECT_MS | 15000 |
| failed-ms / --failed-ms | C3_FAILED_MS | 20000 |
| removed-ms / --removed-ms | C3_REMOVED_MS | 30000 |
| control-timeout-ms / --control-timeout-ms | C3_RPC_MS | 1000 |

Valores entre 100 e 3.600.000 ms. Exigir heartbeat < suspect < failed < removed e orçamento de RPC < suspect. Argumentos sobrescrevem configuração; configurações/argumentos fornecidos sobrescrevem o ambiente; ausência mantém o ambiente ou padrão. Para testes acelerados, a confirmação continua exigindo 1 s entre sondagens.

Para observar queda, obtenha o PID do processo iniciado por você e use `kill -9 <pid>`. Logs mostram `Membership <NodeID> ALIVE -> SUSPECT`, seguido por FAILED e REMOVED. Não mate processos de outros testes/usuários.

```bash
make test
python3 tests/c2/integration.py --bin-dir bin
python3 tests/c3/integration.py --bin-dir bin
python3 tests/c3/failures.py --bin-dir bin
python3 tests/c3/failures.py --bin-dir bin --real-time
```

## 7. Evidências e limitações

Execuções desta revisão: build C2x com warnings estritos e -Werror; `make test`; C1 10/10 em porta 55401; teste legado 14/14 em 55431; C2 próprio 20/20 em 55451/55452/55453; integração TCP C2 46 verificações em `/tmp/pd-c2-integration-6p5pd6qh`; Chord TCP com três Super Peers; suíte C3 acelerada com 13 verificações em `/tmp/pd-c3-failures-rz51l3mw`. Os testes locais injetam tempo e não substituem TCP.

Também foram executados builds isolados ASan/UBSan e ThreadSanitizer, testes locais e suíte TCP de falha com cinco Super Peers: 13 verificações, sem diagnósticos. Evidências ASan/UBSan: `/tmp/pd-c3-failures-qwp9kkhj`; TSan: `/tmp/pd-c3-failures-lv7150rv`. SIGKILL não executa o destrutor de um processo, portanto não equivale a uma prova de ausência de vazamentos nesse processo morto. A execução com prazos reais 5/15/20/30 s passou 13 verificações em `/tmp/pd-c3-failures-87eu7zs4`.

Análise GCC -fanalyzer/-Werror dos módulos membership, Gossip, heartbeat, rede, RPC e Chord passou. A análise completa falhou com diagnóstico analyzer-malloc-leak em `metadata.c`, preexistente e não modificado nesta entrega. Não foi declarado que a análise completa passou; o diagnóstico precisa de investigação própria.

Roteiro disponível do professor: `C2_PORT=55421 C2_PEER_PORT=55422 bash redvidassobretrabalhodepd/run.sh` resultou em 26 PASS e 1 FAIL. A falha é download por `copia.pdf`, que tem exatamente o ObjectID de `data/documento_a.pdf` (`40a4fb67...`). O comportamento C2 conserva o primeiro nome de um ObjectID, sem aliases. Não removemos o arquivo nem alteramos o roteiro para esconder o caso. O log está em `/tmp/bittorrent-tests/c2-pdf.SLkXOz/c2.log`. Não afirmamos aprovação de um roteiro original diferente da cópia disponível.

A primeira tentativa do roteiro revelou sobrescrita de linhas por stdout compartilhado: o Super Peer agora usa O_APPEND quando stdout é arquivo regular. A primeira execução do roteiro próprio C2 revelou a janela listener/IPC: a espera curta de prontidão foi acrescentada à CLI. O Makefile agora tem all como alvo padrão; make -B não inicia o alvo deps acidentalmente.

Não foram demonstradas disponibilidade de 99,5%, média O(log N), sincronização de metadados inferior a dois segundos, consistência forte distribuída, eleição ou recuperação de índices após queda. O detector trabalha com falhas por parada e timeouts: uma partição pode provocar suspeita/falha local. Eventos são eventual e deterministicamente mesclados, não consensuais.
