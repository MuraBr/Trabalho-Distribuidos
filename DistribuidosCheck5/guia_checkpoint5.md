# Guia de arquivos C — Checkpoint 5

Esta pasta reúne uma cópia executável dos fontes do projeto necessários ao Checkpoint 5, seus testes e documentação. O C5 acrescenta journal durável para transações, publicação ANNOUNCE coordenada por Two-Phase Commit (2PC), recuperação incremental de estado (IST) e integração desses fluxos ao armazenamento/cache e à aplicação Peer/Super Peer.

## Arquivos para compilar e executar

- `peer.c`, `file_client.c` e `local_control.c`: cliente Peer, upload/download e interface local.
- `superpeer.c`, `c4_cluster.c` e `c4_log.c`: integração do Super Peer com a coordenação C4/C5, replicação e log de operações.
- `c4_log.c`/`c4_log.h`: log de operações e journal transacional lateral `.txn`.
- `rpc.c`/`rpc.h`, `protocol.c`/`protocol.h`, `network.c`/`network.h` e `wire.h`: requisições correlacionadas, mensagens e transporte TCP.
- `metadata.*`, `directory.*`, `storage.*`, `content.*` e `compression.*`: catálogo, armazenamento, chunks, compressão e cache LFU.
- `membership.*`, `gossip.*`, `heartbeat.*`, `chord.*`, `chord_network.*` e `app_config.*`: dependências de descoberta, associação e configuração usadas pelos executáveis.
- Os demais arquivos `.c` e `.h` na raiz são dependências compartilhadas listadas pelos alvos do `Makefile`.
- `Makefile`: compila `bin/peer`, `bin/superpeer` e os aliases `bin/client` e `bin/node`.
- `tests/c5/`: runner legado, testes locais do journal/cache, integração TCP 2PC/IST e suporte de compatibilidade.
- `tests/config/c5.conf`: inventário dos três Super Peers usados na integração.
- `tests/c3/config.py`, `tests/common.sh`, `tests/env.sh` e `tests/generate_fixtures.sh`: utilitários chamados pelos testes C5.

## Requisitos e compilação

Use Bash em Linux ou WSL, GCC com suporte a C2x, GNU Make, Python 3, pthreads, OpenSSL/libcrypto, zlib e LZ4. A partir desta pasta:

```bash
make all
```

Se os headers de desenvolvimento OpenSSL ou LZ4 estiverem ausentes em Debian/Ubuntu, `make deps` baixa e extrai os pacotes em `.deps` sem instalá-los no sistema; isso exige acesso aos repositórios da distribuição.

## Executar os testes C5

O teste integrado compila com `-Werror`, executa os testes locais do journal e da cache e, em seguida, inicia Super Peers e Peers reais por TCP:

```bash
bash tests/c5/run_integrated.sh
```

O alvo equivalente do Make é `make test-c5`. Ele verifica o journal transacional, replicação ANNOUNCE via 2PC, recuperação IST, mensagens ABORT, paginação/cursor/gap e transferência de conteúdo. Os processos e logs temporários são administrados pelo script Python.

O runner legado do professor usa uma CLI antiga, portanto deve ser executado pela camada de compatibilidade incluída. Prepare o Bash e rode:

```bash
source tests/c5/compat_env.sh
bash ./tests/c5/run.sh
```

O `source` prepara adaptadores que traduzem as chamadas antigas para a CLI atual e executam operações reais nos processos Peer/Super Peer. Sem essa preparação, `bash tests/c5/run.sh` sozinho chama a CLI atual com argumentos antigos e produz falsos negativos em PREPARE, COMMIT e LFU. O fluxo usa as portas 5101–5103 e guarda evidências em uma pasta temporária indicada na saída. Para desativar na sessão atual, use `unset BASH_ENV C5_COMPAT_ROOT`. Não rode este fluxo e o teste integrado simultaneamente, pois ambos podem usar as mesmas portas. `tests/c5/COMPATIBILIDADE.md` detalha a adaptação.

## Comportamento implementado

O ANNOUNCE é preparado nas réplicas antes da decisão. PREPARE e decisões são registrados de forma durável no journal `.txn`; após COMMIT, a entrada é aplicada ao log e ao catálogo. ABORT deixa tombstone para impedir a aceitação de PREPARE atrasado. A recuperação valida os registros persistidos e reaplica commits ainda ausentes do log de operações.

O IST transfere páginas do sufixo de log que falta, usando índice e versão como cursor. O nó receptor valida e aplica as entradas e pode pedir o lote seguinte. O armazenamento do Super Peer atende upload/download e a consulta por nome pode aquecer a cache LFU.

## Limites e distinção dos testes

Os testes locais chamam as APIs dentro do processo e cobrem o journal/cache isoladamente. A integração TCP inicia processos e verifica as mensagens e transferências indicadas acima. A aprovação desses cenários não demonstra consenso completo, consistência forte sob partição, eleição durável ou comportamento em todas as falhas/interleavings possíveis. A implementação aplica localmente antes de receber todas as confirmações das réplicas, e o termo de eleição permanece em memória.

## Evidências documentadas na origem

Em 05/11/2026, `make test-c5` passou com compilação estrita e integração TCP. Em 06/11/2026, o runner legado e `bash tests/c5/run_integrated.sh` também foram registrados como aprovados no projeto de origem. Esses resultados são o histórico da implementação que esta pasta copia; não afirmam que os testes foram reexecutados nesta cópia.
