# Requisitos do trabalho — Aluno 2

Este documento separa as responsabilidades do Aluno 2 a partir do enunciado do trabalho de Programação Distribuída.

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
- `superpeer.c`: criação do Super Peer e tabela de membros.

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

Esse teste valida `node.c` e `superpeer.c` isoladamente. A integração real ainda precisa exercitar o envio de `JOIN` pelo peer e o registro no Super Peer.
