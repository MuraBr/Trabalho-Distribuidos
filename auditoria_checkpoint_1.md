# Auditoria do checkpoint 1

Revisão executada em 17/09/2026 com base no enunciado, no script de testes e
na explicação enviada pelo professor sobre `PING`/`PONG`.

## Resultado

| Item | Estado | Evidência |
| --- | --- | --- |
| Processo distribuído | Implementado | `script_testes_peer.sh` inicia vários processos de `peer.c` |
| Socket TCP | Implementado | `network_create_server`, `network_accept_client` e `network_connect` |
| Cliente/servidor | Implementado no mesmo peer | modo servidor e modo `--cmd` em `peer.c` |
| Serialização | Implementada | header de 98 bytes serializado campo a campo em `protocol.c` |
| Framing | Implementado | header lido primeiro; `payload_size` delimita o corpo |
| CRC32 | Implementado com zlib | CRC do header canônico e payload; vetor de referência testado |
| Concorrência básica | Implementada | uma thread por conexão; teste com 20 PINGs concorrentes |
| Identificação de nós | Implementada | NodeID SHA-256 de IP, porta e UUID |
| `PING`/`PONG` | Implementado | tipos próprios e payloads textuais de quatro bytes |
| `JOIN`/`ACK` | Implementado | identidade reconstruída, validada e registrada antes do ACK |
| `LEAVE`/`ACK` | Parcial | mensagem e ACK funcionam; remoção do membro ainda não é feita |
| Header inválido | Implementado | versão desconhecida é rejeitada sem resposta |

## Organização dos executáveis

Não existe mais `client.c`. O arquivo `peer.c` contém os dois modos:

```text
bin/node --config arquivo --port porta --name nome
bin/node --cmd ping|join|leave --host IP --port porta
```

Para manter compatibilidade com o script fornecido pelo professor, `make`
cria `bin/client` como link simbólico para `bin/node`. Portanto, os dois nomes
executam exatamente o mesmo código compilado de `peer.c`.

As opções longas e as formas curtas `-c`, `-h` e `-p` são processadas com
`getopt_long`.

## Payload de PING/PONG

```text
PING: type=M_PING, payload_size=4, payload="PING"
PONG: type=M_PONG, payload_size=4, payload="PONG"
```

O terminador NUL das strings C não é enviado. O receptor valida tanto o tipo
quanto os quatro bytes do payload.

## Testes executados

```text
script_testes.sh:      10 aprovados, 0 reprovados
script_testes_peer.sh: 14 aprovados, 0 reprovados
test_protocol:         protocol tests: ok
test_node_superpeer:   node/superpeer tests: ok
```

A compilação também passou com C11, `-Wall`, `-Wextra`, `-Wpedantic`,
`-Wconversion`, `-Wsign-conversion`, `-Wunused-function` e `-Werror`.

As APIs `protocol_calculate_crc32`, `node_id_compare` e
`superpeer_config_init`, que antes não possuíam chamadas nos testes, agora são
exercitadas. Nenhuma função interna (`static`) gera aviso de função não usada.

## Pendências fora da aprovação atual

- `LEAVE` ainda não chama `superpeer_unregister_node`; o script do checkpoint
  verifica somente o ACK.
- Em CRC inválido, a conexão é encerrada; não é possível responder `ERROR`
  usando uma mensagem cujo framing/integridade não pôde ser confiado.
- O conteúdo de `tests/config/c1.conf` ainda não é interpretado; apenas a
  existência e a leitura do arquivo são verificadas.
- Upload, download, chunks, LZ4 e hash de arquivos pertencem ao checkpoint 2.
- Chord, Gossip, heartbeat, eleição e replicação pertencem aos checkpoints
  posteriores.
