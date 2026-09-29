# Requisitos do trabalho — Aluno 2


## Revisão integrada — 28/09/2026

Estado vigente: [refatoração C1/C2](refatoracao_checkpoint_2.md). Upload/download agora são executados pelo **Peer ativo**, selecionado por `--local-peer-port` (padrão 55102), mediante canal Unix privado. A CLI não envia LOOKUP anônimo. PDF é validado pela extensão, sem exigir assinatura, para aceitar as fixtures sintéticas.

`peer.c` e `superpeer.c` têm main exclusivos; a API de membros está em `membership.c`. Ambos os processos persistem UUID e leem configuração real. LEAVE remove membro/disponibilidade. Metadados foram ampliados com proprietário NodeID, versão, compressão e descritores/hashes; ANNOUNCE publica o cadastro completo atomicamente.

Evidências funcionais: C1 10/10, regressão legada 14/14, C2 20/20, runner disponível do professor 16/16 e integração independente 46 verificações. APIs locais e testes de falha de manifest passaram. Checkpoints 3–6 e suas metas globais permanecem **fora desta implementação**.

As seções históricas datadas abaixo documentam entregas anteriores; descrições de main condicional, LEAVE pendente, cliente anônimo ou modelo de metadata inalterado não representam esta revisão.

Este documento separa as responsabilidades do Aluno 2 a partir do enunciado do trabalho de Programação Distribuída.

## Acompanhamento da implementação — 25/09/2026

Padronização de estilo: assinaturas, protótipos, chamadas e condições mantidos em uma única linha em `node.c`, `node.h`, `superpeer.c`, `superpeer.h` e `test_node_superpeer.c`, sem mudança de lógica.

Atualizar este documento a cada avanço do aluno 2, mantendo requisitos, implementação e evidências separados. Guia para apresentação: [explicacao_codigo_aluno_2.md](explicacao_codigo_aluno_2.md).

| Item | Estado atual | Evidência ou pendência |
| --- | --- | --- |
| Configuração, validação IPv4/IPv6, UUID e NodeID SHA-256 | Implementado | `node.c`; teste com hash esperado e entradas inválidas |
| PID e papéis peer/Super Peer | Implementado | `node_init`, `superpeer_create` e testes locais |
| Autorregistro e tabela de membros | Implementado | Vetor dinâmico, contagem inicial 1 |
| Inclusão, atualização e consulta | Implementado | Testes confirmam inclusão e duplicata sem aumentar contagem |
| Remoção pela API local | Implementado | Testes de remoção e proteção do próprio Super Peer |
| Concorrência na tabela | Implementado, cobertura parcial | Mutex; teste de 4 threads com 25 registros cada, total 101; não é prova de ausência de corridas |
| JOIN por TCP e validação do NodeID | Implementado na integração | `peer.c` reconstrói identidade e registra antes do ACK; scripts oficial e peer-to-peer aprovados nesta revisão |
| Remoção via LEAVE | Implementada na integração | `superpeer_app.c` remove membership e disponibilidades |
| Resposta ERROR para CRC inválido | Pendente em relação ao requisito | Recepção falha e conexão é encerrada; não envia ERROR nesse caminho |
| Metadados, ObjectID e chunks (C2) | Implementados e integrados | API local preservada; `directory.c` adapta ANNOUNCE/LOOKUP e resolve localizações pela tabela de membros |
| Chord, Gossip, heartbeat e estados de falha (C3) | Pendente | Apenas ALIVE e last_seen existem; sem temporizador de falhas |
| Log, SMR e estado de eleição (C4) | Pendente | Comparação de NodeIDs disponível e testada, mas sem consenso implementado |
| 2PC e IST (C5) | Pendente | Sem implementação |
| Integração final e metas de desempenho (C6) | Pendente | Sem validação das metas não funcionais |

A configuração padrão de `node_config_init` recebe UUID novo a cada inicialização. O processo de armazenamento do Aluno 1 agora persiste seu UUID em `.peer_storage/<porta>/node.uuid` e reutiliza o mesmo NodeID ao reiniciar; o processo Super Peer também persiste UUID em `.superpeer_storage/<porta>/node.uuid`. O teste local de `node.c` não cobre explicitamente a alteração individual de IP, porta e UUID nem todos os caminhos de erro.

Na integração C2, o Super Peer mantém apenas índice e localização. Os Peers persistem chunks e manifests, reingressam via JOIN e anunciam novamente os documentos ao reiniciar. Essa reconstrução não substitui persistência/replicação do índice do Super Peer, que continua pendente para checkpoints posteriores.

## 1. Requisitos comuns da dupla

O projeto deve:

- ser implementado em linguagem C;
- funcionar em ambiente Linux;
- utilizar POSIX Sockets, TCP/IP e POSIX Threads;
- implementar comunicação cliente/servidor;
- utilizar serialização e framing de mensagens;
- utilizar CRC32 para validar mensagens;
- utilizar SHA-256 para identificadores e integridade de conteúdo;
- integrar-se com a implementação do Aluno 1;
- possuir testes, documentação e demonstração em cada checkpoint.

O sistema é um P2P híbrido para compartilhamento distribuído de documentos PDF, com peers e Super Peers.

### Header das mensagens

Todas as mensagens devem possuir os seguintes campos:

- versão do protocolo;
- tipo da mensagem;
- NodeID de origem;
- NodeID de destino;
- TransactionID;
- timestamp;
- tamanho do payload;
- checksum CRC32.

O receptor deve usar o tamanho do payload para determinar exatamente onde uma mensagem termina e a próxima começa.

Na integração atual, `JOIN` e `ACK` utilizam um payload de 64 bytes com IP
textual (46 bytes), porta em ordem de rede (2 bytes) e UUID (16 bytes).

### Integridade e identificadores

- CRC32: valida a mensagem completa, considerando header e payload;
- SHA-256: valida documentos e demais conteúdos definidos pelo protocolo;
- NodeID: possui 32 bytes e é calculado conceitualmente como `SHA256(IP || Porta || UUID)`;
- ObjectID: é calculado como `SHA256(Arquivo)`;
- TransactionID: possui 16 bytes e deve ser tratado de forma consistente pelos dois alunos.

### Bibliotecas externas utilizadas

- O CRC32 é fornecido pela biblioteca `zlib` (`crc32()`), sem código manual de CRC no projeto.
- O SHA-256 é fornecido pela `libcrypto` do OpenSSL (`SHA256()`), chamado diretamente em `node.c`, onde o `NodeID` é calculado.
- Neste ambiente, o link usa explicitamente `libcrypto.so.3`, que está instalada mesmo sem os headers de desenvolvimento do OpenSSL.

## 2. Responsabilidades do Aluno 2

### Arquivos principais

- `common.h`: constantes compartilhadas entre identidade e protocolo;
- `node.c`: configuração, UUID, NodeID e informações do processo;
- `superpeer.c`: main exclusivo; `membership.c`: criação do Super Peer e tabela de membros; `superpeer_app.c` implementa os handlers de rede da integração.

## 3. Checkpoint 1 — identificação e Super Peer básico

O Aluno 2 deve implementar:

- geração ou gerenciamento do NodeID;
- configuração do nó;
- validação de IP e porta;
- geração ou recebimento de UUID;
- identificação do processo com `pid_t`;
- distinção entre peer e Super Peer;
- criação do Super Peer;
- registro do próprio nó;
- tabela básica de membros;
- inclusão de novos nós;
- atualização de membros já registrados;
- remoção de membros.

O Super Peer deve conseguir receber do Aluno 1 um `JOIN`, reconstruir o nó a partir do payload, validar o NodeID e registrá-lo na tabela de membros.

### Verificações do checkpoint 1

- NodeID determinístico para a mesma configuração;
- NodeID diferente quando IP, porta ou UUID mudarem;
- processo identificado corretamente;
- Super Peer criado com seu próprio nó registrado;
- novo peer registrado após `JOIN`;
- `member_count` atualizado corretamente;
- registro duplicado tratado como atualização;
- nó inconsistente rejeitado;
- ausência de segmentation fault e de condições de corrida na tabela.

## 4. Checkpoint 2 — metadados

Responsabilidades do Aluno 2:

- gerenciamento de metadados;
- hash table;
- cálculo do ObjectID;
- registro dos chunks disponíveis;
- associação entre documento, chunks e peers.

O ObjectID deve ser derivado do conteúdo do arquivo usando SHA-256.

## 5. Checkpoint 3 — membership, Gossip e Chord

Responsabilidades do Aluno 2:

- successor;
- predecessor;
- finger table;
- operação de `lookup`;
- entrada (`join`) na DHT;
- `stabilize`;
- `notify`;
- `fix_fingers`;
- Gossip Protocol;
- heartbeat;
- detecção de falhas;
- manutenção dos estados de membership.

Estados previstos:

```text
ALIVE → SUSPECT → FAILED → REMOVED
```

## 6. Checkpoint 4 — consenso e replicação

Responsabilidades do Aluno 2:

- operation log;
- log index;
- version;
- checksum do log;
- integração da máquina de estados replicada (SMR);
- manutenção do estado necessário para a eleição e a recuperação.

O maior NodeID deve possuir a maior prioridade no algoritmo de eleição Bully.

## 7. Checkpoint 5 — 2PC e transferência de estado

Responsabilidades do Aluno 2:

- Two-Phase Commit;
- mensagens `PREPARE`, `COMMIT` e `ABORT`;
- Incremental State Transfer;
- recuperação de versões ausentes;
- sincronização de estado entre Super Peers.

## 8. Checkpoint 6 — integração final

O Aluno 2 deve demonstrar:

- DHT-Chord;
- Gossip;
- heartbeats;
- detecção de falhas;
- eleição Bully;
- SMR;
- 2PC;
- IST;
- integração com upload, download, cache e réplicas do Aluno 1.

## 9. Testes de integração com o Aluno 1

O Super Peer deve:

1. receber um `JOIN` via TCP;
2. desserializar o header e validar o CRC32;
3. interpretar o payload de configuração do nó;
4. reconstruir o `Node` com `node_init()`;
5. confirmar que o NodeID calculado coincide com `Header.source_node`;
6. chamar `superpeer_register_node()`;
7. responder `ACK` somente após o registro;
8. responder `ERROR` em caso de payload inválido, NodeID inconsistente ou checksum incorreto.

Para testar o sentido inverso, o Super Peer também deve conseguir abrir uma conexão com um peer e enviar uma mensagem utilizando a mesma API de protocolo do Aluno 1.

Não se deve enviar diretamente uma `struct Node` ou uma `struct SuperPeer` pela rede. Apenas campos definidos no formato de protocolo devem ser serializados.

## 10. Requisitos não funcionais

O trabalho apresenta como metas:

- disponibilidade mínima de 99,5%;
- escalabilidade sem reconfiguração manual;
- consistência forte dos metadados;
- lookup médio em `O(log N)`;
- sincronização incremental em menos de 2 segundos;
- throughput de compressão LZ4 superior a 500 MB/s;
- eleição em menos de 5 segundos.

## 11. Testes atuais

O teste unitário existente pode ser compilado com:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -pthread \
    node.c superpeer.c test_node_superpeer.c \
    -Wl,-l:libcrypto.so.3 -o test_node_superpeer
```

Execução:

```bash
./test_node_superpeer
```

Esse teste valida `node.c` e `superpeer.c` isoladamente. Em 17/09/2026, os seis cenários passaram com a mensagem `node/superpeer tests: ok`. Os comentários e a documentação foram revisados sem alterar a lógica.

Para não sobrescrever o executável versionado, pode-se usar `-o /tmp/aluno2-test` e executar `/tmp/aluno2-test`.

O `script_testes.sh` exercita protocolo e comunicação C1, incluindo JOIN/ACK, mas não executa `test_node_superpeer.c`. Seu teste LEAVE/ACK não comprova remoção da tabela. Execute as duas suítes para cobrir os dois escopos; a suíte de rede não foi reexecutada nesta revisão documental.

## Verificação após integração com origin/main — 19/09/2026

- `make -B test`: compilação concluída; execução de sockets bloqueada pelo sandbox. `make test` fora do sandbox: passou.
- `bash script_testes.sh`: integração TCP, 10 verificações aprovadas.
- `bash script_testes_peer.sh`: primeira execução com 13 aprovações e uma falha na contagem de logs de PINGs concorrentes; repetição com 14 aprovações. Pendente investigar a intermitência dessa verificação; não considerar estabilidade comprovada.
- A suíte específica da API local de node/superpeer não foi executada nesta integração.

## Checkpoint 2 — implementação local em 24/09/2026

- Implementados: ObjectID SHA-256 incremental de arquivo, hash table com resolução de colisões, cadastro/consulta/remoção de documentos, associação de chunks a múltiplos peers, deduplicação e exclusão de disponibilidade.
- A API usa mutex, cópias de saída e armazenamento esparso. Os chunks usam 4 MiB de conteúdo original; tamanho e quantidade são representados com `uint64_t`.
- O módulo de metadados é independente; `bin/superpeer` cria uma instância duradoura e `directory.c` resolve NodeIDs pela API de membership existente.
- Evidências: `make test-aluno2` e `make test` passaram; suíte C2 com AddressSanitizer/UndefinedBehaviorSanitizer passou fora do sandbox. Na verificação final, o sandbox bloqueou o socket da suíte de protocolo; `make test` passou fora dele. SHA-256 conhecido, colisões, duplicatas, remoção, fronteiras e oito threads cobertos. Não equivale a prova de ausência de corridas.
- Integração posterior: `superpeer_app.c` e `directory.c` chamam a API, validam payloads de `ANNOUNCE`/`LOOKUP` e os fluxos de upload/consulta/download via rede passaram no script C2. O índice continua sem persistência ou consistência distribuída.
- Handoff e contrato detalhado: [integracao_checkpoint_2_aluno_2.md](integracao_checkpoint_2_aluno_2.md). Nenhum fluxo de upload/download ou protocolo do aluno 1 foi implementado aqui.

## Verificação da integração atual — 25/09/2026

A lista anterior registra o estado da entrega local em 24/09. Na integração atual, `directory.c` usa a API de metadados sem alterar `metadata.c`; os handlers do Super Peer aceitam `ANNOUNCE` e `LOOKUP`. `script_testes_c2.sh` passou 20/20, `script_testes.sh` passou 10/10 e `script_testes_peer.sh` passou 14/14. O índice do Super Peer permanece volátil, sem replicação distribuída.

## Organização dos executáveis — 27/09/2026

`bin/superpeer` usa o `main` de `superpeer.c` (habilitado por `SUPERPEER_EXECUTABLE`) e a implementação do servidor em `superpeer_app.c`. `bin/peer` usa o `main` de `peer.c`. A API isolada de membros continua compilável sem `main` para `test_node_superpeer.c`. Após a reorganização, passaram `make test-aluno2`, `script_testes.sh` (10/10), `script_testes_peer.sh` (14/14) e `script_testes_c2.sh` (20/20). As pendências funcionais anteriores não mudaram.
