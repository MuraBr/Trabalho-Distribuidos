# Checkpoint 4 — implementação de eleição e replicação

Este documento resume a implementação integrada do Checkpoint 4, cobrindo as responsabilidades dos dois alunos descritas no enunciado. As responsabilidades foram conectadas no fluxo do Super Peer para que a eleição e o log replicado operem sobre os mesmos membros e operações.

## O que foi implementado

### Eleição Bully — Aluno 1

- Os Super Peers usam o NodeID completo para definir prioridade: o maior NodeID ativo tem prioridade mais alta.
- O candidato envia `M_ELECTION` aos Super Peers ativos com NodeID maior.
- O receptor valida o membro e os 24 bytes do payload, responde `M_OK` e inicia sua própria eleição.
- Quando não há resposta de um candidato superior, o nó anuncia `M_COORDINATOR` aos membros conhecidos.
- A integração com a membership C3 inicia uma nova eleição quando o coordenador passa a `FAILED`.
- Um nó que aprende o novo coordenador sincroniza as entradas de log que ainda não possui antes de retomar a operação como líder.

### Log replicado e recuperação — Aluno 2

- `c4_log.c` mantém o arquivo append-only `c4-operation.log` em cada Super Peer.
- Cada registro contém magic `C4LG`, índice e versão de 64 bits, TransactionID de 16 bytes, tamanho do payload e CRC32.
- O CRC é calculado sobre o cabeçalho com o campo checksum zerado e o payload. Na abertura, os registros são validados; uma cauda incompleta é descartada e as operações válidas são reaplicadas ao diretório local.
- O TransactionID idêntico com o mesmo conteúdo é idempotente. Reutilizá-lo com conteúdo diferente resulta em conflito.
- `M_SNAPSHOT` consulta um índice e permite recuperar entradas ausentes. `M_STATE_TRANSFER` envia registros às réplicas, que validam e aplicam o ANNOUNCE ao diretório.
- O coordenador recebe ANNOUNCE diretamente do Peer ou por encaminhamento de outro Super Peer, grava a operação e a propaga às réplicas conhecidas.

## Mensagens C4

| Mensagem | Payload | Uso |
| --- | --- | --- |
| `M_ELECTION` (15) | termo, último índice e última versão (24 bytes) | Solicita participação na eleição |
| `M_OK` (16) | vazio | Confirma recebimento de `M_ELECTION` |
| `M_COORDINATOR` (17) | termo e último índice (16 bytes) | Anuncia o coordenador eleito |
| `M_SNAPSHOT` (18) | índice solicitado (u64) | Solicita uma entrada do log |
| `M_STATE_TRANSFER` (19) | registro `C4LG` | Replica uma entrada |

As mensagens usam o framing existente do projeto, incluindo TransactionID e CRC32 de transporte. O registro do log também tem seu próprio checksum.

## Como executar

Na raiz do repositório:

```bash
bash runc4.sh
```

O script compila os executáveis com `-Werror`, executa o teste local do log e roda a integração TCP com três Super Peers e um Peer. Para executar somente o teste local do log:

```bash
make test-c4-local
```

Para rodar as suítes locais do projeto, incluindo C4:

```bash
make test CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'
```

## Cobertura executada

`bash runc4.sh` passou build estrito, teste local de persistência/ordem/idempotência/checksum/serialização e 11 verificações TCP. O cenário verificou eleição pelo maior NodeID, JOIN e upload de um Peer, ANNOUNCE persistido nas réplicas, parada do coordenador, reeleição após `FAILED`, preservação dos logs sobreviventes e uma nova operação no coordenador eleito.

Também passaram `make test` com as mesmas opções `-Werror` e a integração de regressão C2 (`python3 tests/c2/integration.py`), com 46 verificações. O teste local verifica a API do log; o roteiro C4 e a integração C2 exercitam processos e mensagens TCP.

## Limites conhecidos

- O teste de eleição cobre três Super Peers e falha por parada. Não demonstra comportamento sob partições de rede nem garante menos de cinco segundos para grupos maiores ou com vários nós lentos.
- O coordenador aplica a operação local antes de receber confirmação de todas as réplicas. Uma interrupção durante a propagação pode deixar logs ou diretórios parcialmente atualizados.
- O termo da eleição é mantido em memória e não sobrevive ao reinício.
- Consistência forte, disponibilidade de 99,5% e sincronização incremental abaixo de dois segundos não foram demonstradas.
- `PREPARE`/`COMMIT`/`ABORT` do 2PC e Incremental State Transfer (IST) são responsabilidades do Checkpoint 5 e não estão concluídos neste checkpoint.
