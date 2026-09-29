# Protocolo e armazenamento — Checkpoint 2

## Processos e camadas

`bin/superpeer` executa identidade, membership e índice de metadados. `bin/peer` possui dois papéis: no modo `serve`, armazena e serve chunks; nos modos `upload` e `download`, coordena uma operação do usuário. O download consulta o Super Peer e transfere diretamente com os Peers anunciados.

| Camada | Módulos |
|---|---|
| Aplicação | `peer.c`, `superpeer.c`, `superpeer_app.c` |
| Serviço | `file_client.c`, `peer_service.c`, `rpc.c`, `concurrent_server.c` |
| Metadados | `directory.c`, `metadata.c`, API de membros em `superpeer.c` |
| Compressão/conteúdo | `compression.c`, `content.c` |
| Armazenamento | `storage.c` |
| Protocolo/rede | `transfer_protocol.c`, `protocol.c`, `network.c` |

## Header e framing

O header permanece com 98 bytes: versão (1), tipo (1), origem (32), destino (32), TransactionID (16), timestamp (8), tamanho (4) e CRC32 (4). O payload máximo é 5 MiB. O receptor primeiro lê o header completo, valida versão/tipo/tamanho, lê exatamente o payload e então valida o CRC32 da mensagem inteira.

Respostas copiam exatamente o TransactionID da requisição. Novas requisições usam 8 bytes de timestamp, 4 bytes iniciais do NodeID e contador atômico de 4 bytes.

## Tipos de mensagem

Valores 0 a 9 estão ativos: `JOIN`, `ACK`, `ERROR`, `PING`, `PONG`, `LEAVE`, `LOOKUP`, `STORE`, `DOWNLOAD_REQ` e `DOWNLOAD_REP`. Valores 10 a 19 já estão fixados para `PREPARE`, `COMMIT`, `ABORT`, `HEARTBEAT`, `GOSSIP`, `ELECTION`, `OK`, `COORDINATOR`, `SNAPSHOT` e `STATE_TRANSFER`; enquanto não tiverem handlers, recebem `ERROR`.

### Documento

Usado por `STORE/BEGIN` e `STORE/ANNOUNCE`:

| Campo | Bytes |
|---|---:|
| operação | 1 |
| ObjectID | 32 |
| tamanho original | 8 |
| quantidade de chunks | 8 |
| algoritmo (`1 = LZ4`) | 1 |
| tamanho do nome | 2 |
| nome, sem NUL | variável |

### Chunk

Usado por `STORE/CHUNK` e `DOWNLOAD_REP`:

| Campo | Bytes |
|---|---:|
| operação | 1 |
| ObjectID | 32 |
| índice | 8 |
| offset | 8 |
| tamanho original | 4 |
| tamanho comprimido | 4 |
| SHA-256 do conteúdo original | 32 |
| conteúdo LZ4 | variável |

### Operações restantes

- `STORE/COMMIT`: operação (1) + ObjectID (32).
- `LOOKUP`: seletor (`ObjectID` ou nome), tamanho de 2 bytes e valor.
- Resultado de lookup: descritor do documento seguido, para cada chunk, pela contagem de Peers e por entradas NodeID/IP/porta.
- `DOWNLOAD_REQ`: operação de chunk (1), ObjectID (32) e índice (8).

Todos os inteiros multibyte usam big-endian. Decodificadores apontam `TransferChunk.data` para dentro do payload recebido; o ponteiro deixa de ser válido ao chamar `message_free`. Encoders e funções LZ4 que retornam buffers transferem a propriedade ao chamador, que usa `free`. `transfer_lookup_result_free` libera todas as listas de localizações.

## Estado e armazenamento

Estados: `CREATED`, `QUEUED`, `STARTED`, `TRANSFERRING`, `VERIFYING`, `FINISHED`; `REPLICATED` está reservado.

Cada Peer usa `.peer_storage/<porta>`:

```text
.peer_storage/<porta>/
├── node.uuid
├── pending/<objectid>/
│   ├── manifest.bin
│   └── chunk-<indice>.lz4
└── objects/<objectid>/
    ├── manifest.bin
    └── chunk-<indice>.lz4
```

O manifest possui magic, versão, documento, proprietário, timestamp, estado e descritores dos chunks. Arquivos temporários são sincronizados com `fsync` e renomeados. No COMMIT, todos os chunks são descomprimidos em ordem e o SHA-256 final precisa ser igual ao ObjectID. Somente depois o diretório passa de `pending` a `objects` e é anunciado.

Manifests finalizados e pendentes válidos são carregados na inicialização. Um BEGIN compatível retoma o registro existente; chunks repetidos precisam ter os mesmos tamanhos e hash. Upload de objeto finalizado é idempotente.

`node.uuid` preserva a identidade do Peer de armazenamento entre reinicializações. Antes de reanunciar objetos finalizados, o Peer verifica descritores, arquivos de chunk, hashes e ObjectID completo.

## Concorrência e falhas

O número de workers é o mínimo entre CPUs, chunks e 8. `PEER_TRANSFER_THREADS` aceita 1 a 32. Cada worker abre sua própria conexão TCP, upload usa `pread` e download usa `pwrite`. O primeiro erro impede distribuição de novas tarefas; todas as threads iniciadas são aguardadas.

O download tenta, em ordem, todos os Peers anunciados para um chunk. O destino é escrito em `<destino>.part`, sincronizado e verificado pelo ObjectID. `link` publica sem sobrescrever um caminho existente e o arquivo parcial é removido depois.

Nomes recebidos não formam caminhos de armazenamento interno. O nome original só pode ser usado como destino escolhido pelo usuário; o catálogo físico é indexado pelo ObjectID.

## Comandos

```bash
./bin/superpeer --port 55101 --name superpeer
./bin/peer serve 55102 127.0.0.1 55101
./bin/peer upload arquivo.pdf 127.0.0.1 55102
./bin/peer download arquivo.pdf copia.pdf 127.0.0.1 55101
./bin/peer benchmark arquivo.pdf
./bin/client --cmd upload --file arquivo.pdf --host 127.0.0.1 --port 55102
./bin/client --cmd download --file arquivo.pdf --output copia.pdf --host 127.0.0.1 --port 55101
```

## Limitações declaradas

O índice do Super Peer é volátil. Não há réplica automática, DHT, Gossip, heartbeat, limpeza de localizações mortas, SMR, 2PC, LFU ou IST neste checkpoint. A meta de 500 MB/s é informativa e dependente de hardware; `peer benchmark` mede apenas a compressão LZ4 e exibe duração, bytes e throughput, sem transformar o resultado em condição funcional.
