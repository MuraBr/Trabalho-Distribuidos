# Checkpoint 2 — contrato de integração com o Aluno 2

## Estado atual

A API local de `metadata.c` está integrada ao processo `bin/superpeer` por `directory.c`. Sua estrutura pública permanece inalterada: documento, ObjectID, tamanho, quantidade de chunks e associações `(ObjectID, índice, NodeID)`. Metadados adicionais do enunciado ficam no manifest persistente do Peer, sem serem apresentados como parte de `metadata.c`.

O fluxo em rede está implementado:

1. o Peer faz `JOIN` e é validado/registrado por `superpeer.c`;
2. após publicar um documento localmente, envia `STORE/ANNOUNCE`;
3. `superpeer_app.c` confirma que o `Header.source_node` pertence à tabela de membros;
4. `directory.c` chama `metadata_register_document` e `metadata_register_chunk`;
5. em `LOOKUP`, consulta `metadata_find_document` e `metadata_chunk_peers`;
6. resolve cada NodeID com `superpeer_find_member` e devolve IP/porta ao cliente;
7. os bytes dos chunks seguem diretamente entre cliente e Peers, sem passar pelo Super Peer.

## Convenções compartilhadas

- ObjectID: SHA-256 dos bytes do documento original inteiro.
- Chunk: 4 MiB (`METADATA_CHUNK_SIZE`), índice iniciado em zero.
- NodeID e ObjectID são 32 bytes binários na rede.
- Inteiros multibyte são big-endian.
- Structs C e ponteiros nunca são transmitidos diretamente.
- `metadata_chunk_peers` devolve memória que `directory.c` libera com `free()`.
- O mesmo documento e associação podem ser anunciados novamente; o registro é idempotente.

## Limites da integração atual

`metadata.c` continua em memória. Ao reiniciar somente o Super Peer, seu índice é reconstruído quando Peers de armazenamento reiniciam e anunciam seus manifests; não existe ainda solicitação periódica de reanúncio. LEAVE não remove associações e não há detecção automática de Peer morto: durante download, o cliente tenta a próxima localização retornada.

Nome duplicado para ObjectIDs diferentes é detectado em `directory.c` e retorna erro de ambiguidade. A API atual preserva o primeiro nome associado a um ObjectID. Hash individual, compressão, timestamp, estado e tamanhos dos chunks permanecem nos manifests dos Peers.

Chord, Gossip, replicação automática, consenso distribuído, 2PC e IST não fazem parte desta entrega e não são declarados como implementados.

## Evidências

- `make test-aluno2`: testa `node.c`, `superpeer.c` e a API local de metadados.
- `script_testes.sh`: mantém 10/10 no Checkpoint 1.
- `script_testes_peer.sh`: mantém 14/14 na comunicação anterior.
- `script_testes_c2.sh`: testa dois Peers, upload pequeno e multichunk, lookup, download, fallback, reinicialização, novo anúncio, idempotência e rejeições básicas.
