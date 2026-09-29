# Requisitos do trabalho — Aluno 1


## Revisão integrada — 28/09/2026

Estado vigente: [refatoração C1/C2](refatoracao_checkpoint_2.md). Upload/download agora são executados pelo **Peer ativo**, selecionado por `--local-peer-port` (padrão 55102), mediante canal Unix privado. A CLI não envia LOOKUP anônimo. PDF é validado pela extensão, sem exigir assinatura, para aceitar as fixtures sintéticas.

`peer.c` e `superpeer.c` têm main exclusivos; a API de membros está em `membership.c`. Ambos os processos persistem UUID e leem configuração real. LEAVE remove membro/disponibilidade. Metadados foram ampliados com proprietário NodeID, versão, compressão e descritores/hashes; ANNOUNCE publica o cadastro completo atomicamente.

Evidências funcionais: C1 10/10, regressão legada 14/14, C2 20/20, runner disponível do professor 16/16 e integração independente 46 verificações. APIs locais e testes de falha de manifest passaram. Checkpoints 3–6 e suas metas globais permanecem **fora desta implementação**.

As seções históricas datadas abaixo documentam entregas anteriores; descrições de main condicional, LEAVE pendente, cliente anônimo ou modelo de metadata inalterado não representam esta revisão.

Este documento separa as responsabilidades do Aluno 1 a partir do enunciado do trabalho de Programação Distribuída.

## Estado da implementação — Checkpoint 2

O Checkpoint 2 está integrado à rede. `bin/superpeer` mantém o índice de metadados e localizações; `bin/peer` armazena, comprime, transfere e valida chunks. `bin/node` aponta para `bin/superpeer` e `bin/client` aponta para `bin/peer`, preservando os comandos dos testes anteriores.

Implementado e testado: upload e download de PDF, ObjectID SHA-256, chunks de 4 MiB, SHA-256 por chunk, LZ4 por chunk, manifests locais, publicação atômica, pools de transferência, lookup no Super Peer, transferência direta entre cliente e Peers, fallback entre localizações, reinicialização com novo anúncio e idempotência por ObjectID. Chord, Gossip, réplica automática, SMR, 2PC, LFU e IST permanecem reservados para checkpoints posteriores.

## 1. Requisitos comuns da dupla

O projeto deve:

- ser implementado em linguagem C;
- funcionar em ambiente Linux;
- utilizar POSIX Sockets, TCP/IP e POSIX Threads;
- implementar comunicação cliente/servidor;
- utilizar serialização e framing de mensagens;
- utilizar CRC32 para validar mensagens;
- utilizar SHA-256 para identificadores e integridade de conteúdo;
- integrar-se com a implementação do Aluno 2;
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
`PING` e `PONG` utilizam, respectivamente, os quatro bytes textuais `PING`
e `PONG`, sem transmitir o terminador NUL.

### Integridade e identificadores

- CRC32: valida a mensagem completa, considerando header e payload;
- SHA-256: valida documentos e demais conteúdos definidos pelo protocolo;
- NodeID: possui 32 bytes e é calculado pelo Aluno 2 conforme `SHA256(IP || Porta || UUID)`;
- TransactionID: possui 16 bytes e deve ser tratado de forma consistente pelos dois alunos.

### Bibliotecas externas utilizadas

- O CRC32 é fornecido pela biblioteca `zlib` (`crc32()`); não há mais uma implementação manual do polinômio no projeto.
- O SHA-256 é fornecido pela `libcrypto` do OpenSSL: `node.c` calcula o NodeID, `metadata.c` calcula o ObjectID incrementalmente e `content.c` valida cada chunk.
- Compressão e descompressão usam a ABI estável da biblioteca externa `liblz4.so.1`; não há implementação manual de LZ4.
- Neste ambiente, o link é feito explicitamente para `libcrypto.so.3`, pois a biblioteca de execução está instalada mesmo sem os headers de desenvolvimento do OpenSSL.

## 2. Responsabilidades do Aluno 1

### Arquivos principais

- `common.h`: constantes compartilhadas entre identidade e protocolo;
- `network.c`: operações de socket TCP;
- `protocol.c`: header, serialização, framing e CRC32;
- `peer.c`: processo do Super Peer e handlers de `JOIN`, `LOOKUP` e `ANNOUNCE`.
- `peer_app.c`: comandos e inicialização do Peer de armazenamento.
- `peer_service.c`: servidor concorrente de upload e download de chunks.
- `concurrent_server.c`: ciclo concorrente compartilhado pelos dois servidores.
- `file_client.c`: coordenação paralela de upload e download.
- `transfer_protocol.c`: formatos wire do Checkpoint 2.
- `storage.c`: manifests, diretórios `pending`/`objects` e publicação atômica.
- `compression.c` e `content.c`: integração com LZ4 e SHA-256.
- `directory.c`: adaptação entre mensagens de rede, `metadata.c` e membros do Super Peer.

Os processos são separados: `bin/superpeer` executa o índice e `bin/peer` executa armazenamento e comandos. Os aliases `bin/node` e `bin/client` mantêm a interface de correção do Checkpoint 1. Não existe um módulo `client.c` na compilação padrão.

## 3. Checkpoint 1 — comunicação básica

O Aluno 1 deve implementar:

- criação de socket;
- `bind`;
- `listen`;
- `accept`;
- `connect`;
- envio com `send`;
- recebimento com `recv`;
- tratamento de envios e recebimentos parciais;
- serialização e desserialização do header;
- framing baseado no tamanho do payload;
- cálculo e validação do CRC32;
- concorrência básica, preferencialmente com POSIX Threads;
- comunicação entre pelo menos dois processos.

O `peer.c` deve conseguir iniciar como servidor e, quando necessário, conectar-se a outro peer como cliente.

O peer deve utilizar o `NodeID` produzido por `node.c`, copiando os 32 bytes
de `Node.id.bytes` para os campos do header.

### Mensagens mínimas

Para a primeira demonstração, devem funcionar pelo menos:

- `JOIN`;
- `ACK`;
- `ERROR`;
- `PING` com payload textual `PING`;
- `PONG` com payload textual `PONG`;
- `LEAVE` com resposta `ACK`.

### Verificações do checkpoint 1

- conexão TCP funcionando;
- mensagem chegando corretamente;
- header interpretado corretamente;
- checksum validado;
- dois processos conversando;
- ausência de segmentation fault;
- sockets fechados em caminhos normais e de erro.

## 4. Checkpoint 2 — arquivos e transferência

Responsabilidades do Aluno 1:

- upload;
- download;
- armazenamento local;
- fragmentação de arquivos;
- SHA-256 do conteúdo;
- compressão e descompressão com LZ4;
- checksum dos blocos;
- transferência paralela.

O tamanho de chunk especificado no trabalho é 4 MB.

Nesta implementação, “4 MB” é fixado em 4 MiB (`4194304` bytes), compartilhado por `METADATA_CHUNK_SIZE`. O payload máximo do protocolo é 5 MiB para comportar `LZ4_compressBound(4 MiB)` e o descritor do chunk.

Pipeline esperado:

```text
arquivo → SHA-256 → fragmentação → LZ4 → checksum → transferência
```

Estados usados: `CREATED → QUEUED → STARTED → TRANSFERRING → VERIFYING → FINISHED`; `REPLICATED` está reservado. O Super Peer não armazena os bytes do documento. O cliente obtém localizações com `LOOKUP` e baixa os chunks diretamente dos Peers.

## 5. Checkpoint 3 — membership e falhas

Responsabilidades do Aluno 1:

- manutenção de membership local;
- processamento de heartbeats;
- estado `SUSPECT`;
- estado `FAILED`;
- estado `REMOVED`.

Transição esperada:

```text
ALIVE → SUSPECT → FAILED → REMOVED
```

As mensagens de membership devem utilizar o protocolo definido em `protocol.c`.

## 6. Checkpoint 4 — eleição

Responsabilidades do Aluno 1:

- envio da mensagem `ELECTION`;
- recebimento da resposta `OK`;
- envio da mensagem `COORDINATOR`;
- fluxo de recuperação quando o coordenador falhar.

## 7. Checkpoint 5 — cache e transferência

Responsabilidades do Aluno 1:

- implementação do cache LFU;
- identificação de cache hit e cache miss;
- contagem de frequência de uso;
- eviction de entradas;
- transferência de dados usando a camada de rede.

## 8. Checkpoint 6 — integração final

O Aluno 1 deve demonstrar:

- upload;
- download;
- fragmentação em chunks;
- compressão;
- cache;
- réplica e comunicação com os componentes do Aluno 2.

## 9. Testes de integração com o Aluno 2

O peer deve:

1. enviar um `JOIN` contendo seu NodeID e sua configuração serializada;
2. permitir que o Super Peer reconstrua e valide o `Node`;
3. receber `ACK` quando o nó for registrado;
4. receber `ERROR` quando o NodeID, payload ou checksum forem inválidos;
5. manter o mesmo TransactionID na solicitação e na resposta.

Não se deve enviar diretamente uma `struct Node` pela rede. O payload deve possuir um formato de rede documentado, sem depender de padding ou representação interna das structs.

## 10. Requisitos não funcionais

O trabalho apresenta como metas:

- disponibilidade mínima de 99,5%;
- escalabilidade sem reconfiguração manual;
- consistência forte dos metadados;
- lookup médio em `O(log N)`;
- sincronização incremental em menos de 2 segundos;
- throughput de compressão LZ4 superior a 500 MB/s;
- eleição em menos de 5 segundos.

## 11. Comandos iniciais de teste

Compilação:

```bash
make CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'
```

Execução do Checkpoint 2:

```bash
./bin/superpeer --port 55101 --name superpeer
./bin/peer serve 55102 127.0.0.1 55101
./bin/peer upload arquivo.pdf 127.0.0.1 55102
./bin/peer download arquivo.pdf copia.pdf 127.0.0.1 55101
```

Execução dos testes automatizados:

```bash
make
bash script_testes.sh
bash script_testes_peer.sh
bash script_testes_c2.sh
```
