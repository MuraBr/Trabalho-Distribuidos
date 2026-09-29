# Protocolo e armazenamento — Checkpoint 2

## Processos e camadas

`bin/superpeer` executa identidade, membership e índice de metadados. `bin/peer` possui dois papéis: no modo `serve`, armazena e serve chunks; nos modos `upload` e `download`, encaminha uma operação ao serviço Peer ativo por socket Unix privado. O serviço coordena os workers com seu próprio NodeID. O download consulta o Super Peer e transfere diretamente com os Peers anunciados.

| Camada | Módulos |
|---|---|
| Aplicação | `peer.c`, `superpeer.c`, `superpeer_app.c` |
| Serviço | `file_client.c`, `peer_service.c`, `rpc.c`, `concurrent_server.c` |
| Metadados | `directory.c`, `metadata.c`, API de membros em `membership.c` |
| Compressão/conteúdo | `compression.c`, `content.c` |
| Armazenamento | `storage.c` |
| Protocolo/rede | `transfer_protocol.c`, `protocol.c`, `network.c` |

## Header e framing

O header permanece com 98 bytes: versão (1), tipo (1), origem (32), destino (32), TransactionID (16), timestamp (8), tamanho (4) e CRC32 (4). O payload máximo é 5 MiB. O receptor primeiro lê o header completo, valida versão/tipo/tamanho, lê exatamente o payload e então valida o CRC32 da mensagem inteira.

Respostas copiam exatamente o TransactionID da requisição. Novas requisições usam 8 bytes de timestamp em nanossegundos (monotônico lógico dentro do processo), 4 bytes iniciais do NodeID e contador atômico de 4 bytes.

## Tipos de mensagem

Valores 0 a 9 estão ativos: `JOIN`, `ACK`, `ERROR`, `PING`, `PONG`, `LEAVE`, `LOOKUP`, `STORE`, `DOWNLOAD_REQ` e `DOWNLOAD_REP`. Valores 10 a 19 já estão fixados para `PREPARE`, `COMMIT`, `ABORT`, `HEARTBEAT`, `GOSSIP`, `ELECTION`, `OK`, `COORDINATOR`, `SNAPSHOT` e `STATE_TRANSFER`; enquanto não tiverem handlers, recebem `ERROR`.

### Documento

Prefixo usado por `STORE/BEGIN` e `STORE/ANNOUNCE` (o anúncio v2 acrescenta descritores):

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
- Resultado de lookup v2: descritor do documento seguido, para cada chunk, pelo descritor de 56 bytes, contagem de Peers (uint16) e entradas NodeID/IP/porta.
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

## Versionamento dos payloads C2 desta revisão

Header C1 permanece versão 1 e 98 bytes. Códigos de mensagem 0–19 não mudaram. Para distinguir os payloads C2 ampliados, STORE/ANNOUNCE usa operação **5** (antiga 4 não é aceita); DOWNLOAD_REP de metadados usa operação **3** (antiga 1 não é aceita). BEGIN=1, CHUNK=2 e COMMIT=3 em STORE permanecem. DOWNLOAD_REQ/REP de chunk usa operação 2.

Cada descritor tem 56 bytes: índice uint64, offset uint64, tamanho original uint32, tamanho comprimido uint32, SHA-256 de 32 bytes. ANNOUNCE inclui um descritor por chunk após o documento. Lookup inclui descritor antes da lista de peers de cada chunk. Endpoints: NodeID 32, tamanho IP 1, texto IP sem NUL, porta uint16. O receptor valida comprimento total antes das alocações.

ERROR: byte de versão 1 seguido de código: 1 ausente; 2 ambíguo; 3 já existe/incompatível; 4 dados inválidos; 5 não suportado; 6 identidade/acesso inválido; 7 chunks indisponíveis; 8 limite excedido; 9 erro interno. Frames de CRC/header inválidos são encerrados sem ERROR porque a mensagem não é confiável.

C2 exige origem não nula e destino conhecido. PING descobre o destino de uma operação; JOIN aprende a identidade do SP validando seu descritor. RPC verifica TransactionID e origem/destino esperados. Não há autenticação criptográfica.

## Controle local

Socket Unix privado: /tmp/pd-c2-UID/peer-PORTA.sock. Pedido: uint32 operação (1 upload, 0 download), uint32 porta remota, cinco strings com uint32 comprimento e bytes sem NUL: arquivo/seletor, destino (vazio se padrão), IPv4 remoto, reservado vazio, diretório da CLI. Strings têm limite 4095 bytes.

Resposta: uint32 estado e string com uint32 comprimento. UINT32_MAX indica progresso; outro estado encerra a operação (0 sucesso; erro local mapeado para errno). Esse errno é apenas IPC local Linux, não o protocolo TCP. Cada frame tem limite 5 MiB. Arquivos e diretórios privados restringem acesso ao mesmo usuário.

O formato de manifest em disco permanece versão 1 e legível; não houve renumeração dos campos persistentes.
