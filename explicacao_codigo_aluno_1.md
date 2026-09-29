# Explicação da implementação — Aluno 1, Checkpoint 2

## Resultado

A aplicação implementa upload e download de documentos PDF em um P2P híbrido. O Super Peer mantém metadados e localizações; os Peers armazenam chunks comprimidos; o cliente consulta o índice e transfere diretamente com os Peers. A base do Checkpoint 1 continua compatível com os scripts anteriores.

```text
upload:   cliente ──chunks──► Peer ──ANNOUNCE──► Super Peer
lookup:   cliente ──LOOKUP──► Super Peer
download: cliente ◄─chunks─── Peer(s)
```

## Executáveis

- `bin/superpeer`: tem `main` em `superpeer.c` e compila `superpeer_app.c`, `metadata.c` e `directory.c`.
- `bin/peer`: tem `main` em `peer.c` e compila `peer_service.c`, `file_client.c`, `storage.c` e módulos compartilhados.
- `bin/node`: link para `bin/superpeer`, por compatibilidade.
- `bin/client`: link para `bin/peer`, por compatibilidade.

`client.c` permanece apenas como referência histórica e não participa do build padrão.

## Rede e protocolo

`network.c` encapsula `socket`, `bind`, `listen`, `accept`, `connect`, envio completo, recepção exata e encerramento. `send` usa `MSG_NOSIGNAL`; o processo também ignora `SIGPIPE`. Envios e recepções parciais são acumulados.

`protocol.c` serializa manualmente o header de 98 bytes, em big-endian, usa o tamanho do payload como framing e valida CRC32 da zlib. O limite é 5 MiB. `protocol.h` fixa todos os tipos do enunciado; os que pertencem a checkpoints futuros são reconhecidos, porém respondem `ERROR`.

`transfer_protocol.c` define os payloads de documento, chunk, commit, lookup e solicitação de chunk. NodeID, ObjectID e hash de chunk têm 32 bytes; TransactionID tem 16. A descrição byte a byte está em `protocolo_checkpoint_2.md`.

## Identidade e índice

O Peer de armazenamento cria um `Node` e faz JOIN no Super Peer. O servidor reconstrói a configuração, recalcula o NodeID e só registra o membro quando os valores coincidem.

Depois de um COMMIT válido, o Peer envia `STORE/ANNOUNCE`. O Super Peer exige que o NodeID de origem esteja cadastrado. `directory.c` então chama a API inalterada de `metadata.c`: registra o documento e associa todos os chunks ao NodeID. No LOOKUP, resolve esses NodeIDs em IP/porta pela tabela de `superpeer.c`.

## Upload

1. `content_validate_pdf` exige extensão `.pdf` e assinatura `%PDF-` nos primeiros 1024 bytes.
2. `object_id_file` lê incrementalmente e calcula o ObjectID SHA-256 do documento inteiro.
3. O arquivo é dividido em chunks de 4 MiB.
4. Um pool obtém trabalhos por índice; cada worker usa `pread`.
5. Cada chunk recebe SHA-256 do conteúdo original e compressão LZ4 individual.
6. Cada worker abre sua própria conexão e envia `STORE/CHUNK`.
7. O Peer descomprime temporariamente, verifica hash, índice, offset e tamanhos, e grava em `pending`.
8. No COMMIT, recompõe o documento em ordem, recalcula ObjectID e tamanho, publica em `objects` e anuncia ao Super Peer.
9. O cliente só mostra sucesso depois do ACK do anúncio.

Upload repetido do mesmo ObjectID é idempotente. Um upload interrompido deixa manifest e chunks em `pending`; um novo BEGIN compatível reutiliza esse estado.

## Download

1. O cliente envia LOOKUP por nome ou ObjectID.
2. O Super Peer devolve o documento e as localizações disponíveis para cada chunk.
3. O cliente cria `<destino>.part` com exclusividade.
4. Workers pedem chunks diretamente aos Peers e tentam a próxima localização quando uma falha.
5. Cada resposta passa por CRC32, validação do descritor, LZ4, SHA-256 e `pwrite` no offset esperado.
6. O arquivo completo é sincronizado e seu SHA-256 é comparado ao ObjectID.
7. `link` publica sem sobrescrever destino existente; o `.part` é removido.

## Armazenamento e manifest

`storage.c` mantém `.peer_storage/<porta>/pending` e `objects`. Caminhos internos usam apenas o ObjectID hexadecimal, nunca nomes recebidos. O manifest binário versionado registra documento, algoritmo, NodeID proprietário, timestamp, estado e descritores de chunks.

O processo de armazenamento guarda seu UUID em `.peer_storage/<porta>/node.uuid`. Assim, o NodeID proprietário permanece igual após reinicialização na mesma porta.

Chunks e manifests são escritos em temporários, recebem `fsync` e são renomeados. Manifests válidos, inclusive pendentes, são carregados na inicialização; os finalizados têm chunks, SHA-256 individual e ObjectID completo verificados antes do novo anúncio. Documentos finalizados são anunciados novamente depois do JOIN.

## Concorrência

`concurrent_server.c` é compartilhado pelo Peer e pelo Super Peer: cria uma thread destacada por conexão, acompanha os sockets ativos e aguarda os atendimentos antes de destruir o contexto. Upload e download usam pools POSIX; o padrão é o mínimo entre CPUs, chunks e 8. `PEER_TRANSFER_THREADS` permite 1 a 32. A fila de trabalho e o primeiro erro são protegidos por mutex.

Cada worker usa conexão própria. Isso evita compartilhar estado de framing entre threads e permite fallback independente por chunk.

## Bibliotecas externas

- CRC32: zlib.
- NodeID, ObjectID e SHA-256 de chunks: libcrypto.
- Compressão/descompressão: `liblz4.so.1`.

Não há implementação manual desses algoritmos. Como o ambiente possui as bibliotecas de execução sem todos os headers de desenvolvimento, os módulos declaram apenas as funções da ABI estável que utilizam e o Makefile liga versões instaladas.

## Comandos de demonstração

```bash
make
./bin/superpeer --port 55101 --name superpeer
./bin/peer serve 55102 127.0.0.1 55101
./bin/peer serve 55103 127.0.0.1 55101
./bin/peer upload trabalho_2026_SD.pdf 127.0.0.1 55102
./bin/peer download trabalho_2026_SD.pdf copia.pdf 127.0.0.1 55101
./bin/peer benchmark trabalho_2026_SD.pdf
```

## Testes e evidências

- `script_testes.sh`: 10/10, C1.
- `script_testes_peer.sh`: 14/14, integração anterior.
- `script_testes_c2.sh`: 20/20, incluindo PDF multichunk, pool com dois workers, benchmark informativo, downloads por nome e ID, nome ambíguo, comparação byte a byte, fallback, reinicialização, identidade estável, novo anúncio, idempotência e rejeições.
- Compilação validada com `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
- Os dois executáveis passaram por `-fanalyzer`; uma integração de upload/download também foi executada com ASan/UBSan e com ThreadSanitizer sem diagnóstico.

Os testes comprovam os casos executados, não as metas distribuídas futuras. Disponibilidade de 99,5%, consistência forte entre vários Super Peers, lookup Chord O(log N), réplica automática, Gossip, SMR, 2PC, LFU e IST dependem de checkpoints seguintes.
