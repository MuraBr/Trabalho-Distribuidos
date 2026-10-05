# Continuidade do checkpoint 3 para o aluno 1

## Estado recebido em 05/10/2026

O aluno 2 implementou o anel Chord entre Super Peers: sucessor, predecessor, 256 fingers, lookup de responsável, join, stabilize, notify e fix_fingers. `chord.c` é a API local, `chord_network.c` faz as trocas TCP e `superpeer.c` chama a manutenção periódica. `make test-c3-local` verifica a API e `python3 tests/c3/integration.py` verifica três processos pela rede. A consulta de documentos permanece local; não há migração ou replicação dos índices. Heartbeat, Gossip e estados de falha ainda são tarefas abertas.

## Como iniciar e conferir o anel

Em terminais separados, use portas e diretórios de trabalho distintos para preservar o UUID de cada Super Peer:

```sh
./bin/superpeer --port 55101
./bin/superpeer --port 55111 --chord-host 127.0.0.1 --chord-port 55101
./bin/superpeer --port 55121 --chord-host 127.0.0.1 --chord-port 55101
```

O teste de integração cria diretório temporário próprio, inicia três processos e valida o anel após estabilização. A opção `--chord-host` exige `--chord-port`. Um único Super Peer inicia como anel isolado. O `JOIN` antigo, usado pelos Peers para entrar no cadastro local, permanece separado do ingresso Chord.

## Contrato TCP do Chord

Todos os tipos usam o header de 98 bytes, TransactionID e CRC32 de `protocol.c`. Um descritor Chord tem 64 bytes: IP textual e preenchimento zero (46), porta big-endian (2), UUID (16). O NodeID é recalculado por `node_compute_id` a partir do descritor.

| Tipo | Código | Pedido | Resposta |
| --- | ---: | --- | --- |
| `M_CHORD_INFO` | 20 | vazio | 64 bytes do Super Peer consultado |
| `M_CHORD_ROUTE` | 21 | chave de 32 bytes | 1 byte `complete` (0/1) + 64 bytes do próximo salto ou responsável |
| `M_CHORD_PREDECESSOR` | 22 | vazio | 64 bytes do predecessor, ou vazio se desconhecido |
| `M_CHORD_NOTIFY` | 23 | 64 bytes do candidato, com NodeID correspondente no header | `M_ACK` vazio |
| `M_CHORD_FINGER` | 24 | índice de 1 byte (0–255) | 64 bytes do destino dessa entrada; útil para diagnóstico |

Uma mensagem malformada recebe `M_ERROR`. `chord_network_find` resolve o responsável por uma chave de 32 bytes; ele não consulta o índice de documentos. `chord_get_successor` e `chord_get_predecessor` entregam cópias para o chamador, evitando manter o mutex durante operações TCP.

## Próximas tarefas do aluno 1 e pontos de integração

1. **Heartbeat entre Peer e Super Peer.** O enum já contém `M_HEARTBEAT = 13`. Implementar emissor periódico no processo Peer (`peer.c`) e handler no Super Peer (`superpeer.c`). O handler deve validar identidade, responder de modo definido e atualizar `last_seen` do membro sob mutex. O enunciado sugere intervalo de 5 s e timeout de 15 s; definir explicitamente os tempos das transições posteriores antes dos testes.
2. **Estados de membership.** `SuperPeerMemberState` em `superpeer.h` contém apenas `ALIVE`. Acrescentar `SUSPECT`, `FAILED` e `REMOVED` e uma rotina periódica que aplique `ALIVE → SUSPECT → FAILED → REMOVED` por prazo e confirmação. Distinguir saída voluntária `LEAVE` de queda. Evitar remover localizações de chunks durante suspeita transitória; estabelecer em que transição `metadata_remove_peer` será chamado.
3. **Gossip.** O enum já contém `M_GOSSIP = 14`. Definir payload com NodeID, estado, versão/época e origem; serializar inteiros em ordem de rede. Disseminar periodicamente entre Super Peers, rejeitar atualizações antigas e limitar retransmissão. Um nó recuperado deve atualizar o estado por versão, sem ressuscitar registros removidos com mensagens atrasadas.
4. **Falha de Super Peer no anel.** Integrar o detector ao Chord: invalidar sucessor/predecessor/fingers indisponíveis e selecionar alternativa conhecida. Hoje uma falha de RPC interrompe a manutenção e o roteamento; não há recuperação automática. Não segurar `members_mutex` nem o mutex de Chord durante chamadas TCP.
5. **Metadados distribuídos.** O aluno 2 deve combinar com você o contrato de `M_STORE`/`M_LOOKUP` para encaminhar uma chave ObjectID ao responsável retornado por `chord_network_find`, preservar a identidade do Peer proprietário e transferir índice ao entrar/sair um Super Peer. A API local `directory_lookup` não é uma consulta distribuída. Nomes de arquivos exigem uma regra de chave própria, pois não são ObjectIDs.
6. **Testes.** Acrescentar testes de heartbeat válido/inválido, falha sem `LEAVE`, suspeita e recuperação, remoção após timeout, Gossip duplicado/atrasado e falha de sucessor durante lookup. Depois executar `make test`, `python3 tests/c2/integration.py` e `python3 tests/c3/integration.py`. Registrar quais comandos rodaram e seus resultados; testes locais não substituem os de TCP.

## Limites a preservar

- `M_JOIN` e `M_LEAVE` atuais cadastram Peers no Super Peer e atualizam o diretório C2. Não os reutilizar como operações Chord.
- A API de `protocol.c` reconhece `M_HEARTBEAT` e `M_GOSSIP`, mas a presença no enum não significa que seus fluxos estejam implementados.
- Ao alterar `node.c`, `superpeer.c` ou testes do aluno 2, manter comentários explicativos em português e atualizar `requisitos_aluno_2.md` e `explicacao_codigo_aluno_2.md` com comportamento e evidências reais. `requisitos_aluno_1.md` exige instrução explícita do usuário antes de qualquer modificação.
