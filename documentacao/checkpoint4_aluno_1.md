# Checkpoint 4 — participação do Aluno 1

Guia do fluxo de eleição Bully e recuperação do coordenador no sistema integrado.

## Escopo e divisão de responsabilidades

No Checkpoint 4, a responsabilidade atribuída ao Aluno 1 é a participação no protocolo de eleição: enviar `ELECTION`, receber `OK`, anunciar `COORDINATOR` e iniciar a recuperação quando o coordenador falha. A execução está integrada ao `c4_cluster.c` e ao processo `superpeer.c`; portanto, não há uma implementação isolada por aluno em execução. Este guia explica a responsabilidade do Aluno 1 dentro desse fluxo compartilhado.

O log durável, a indexação/versionamento das entradas e a replicação SMR são responsabilidades atribuídas ao Aluno 2 e explicadas no guia correspondente. A eleição usa esses dados para decidir qual estado já existe e recuperar entradas ausentes.

## Fluxo da eleição

1. O detector de membership do C3 muda o estado do coordenador para `FAILED`.
2. Os Super Peers ativos iniciam uma rodada de eleição. Cada candidato compara NodeIDs completos em ordem lexicográfica; o maior NodeID tem maior prioridade.
3. O candidato envia `M_ELECTION` aos membros ativos de prioridade superior, incluindo seu termo e o índice/versão local do log.
4. Um Super Peer superior que recebe a solicitação válida responde com `M_OK` e também inicia a própria eleição.
5. Se o candidato não obtiver resposta de um membro superior, ele se declara coordenador e envia `M_COORDINATOR` aos membros conhecidos.
6. Os demais membros atualizam o coordenador conhecido. O novo líder busca entradas de log que não possui antes de retomar o serviço.

```text
coordenador falha
       |
       v
C3 marca FAILED -> candidatos enviam M_ELECTION aos NodeIDs maiores
                          |
               +----------+----------+
               |                     |
             M_OK                 sem resposta
               |                     |
       nó superior elege-se     candidato vira líder
                                     |
                               M_COORDINATOR
                                     |
                           recuperar entradas ausentes
```

## Mensagens e validação

| Mensagem | Tipo | Payload | Função |
| --- | ---: | --- | --- |
| `M_ELECTION` | 15 | 24 bytes: termo, último índice e última versão | Inicia ou propaga a eleição |
| `M_OK` | 16 | vazio | Confirma que um candidato superior recebeu a eleição |
| `M_COORDINATOR` | 17 | 16 bytes: termo e índice anunciado | Informa quem assumiu a coordenação |

As mensagens usam o framing C1/C2 já existente: header, TransactionID, tamanho do payload e CRC32 de transporte. Os handlers também verificam os tamanhos esperados e se o remetente é um membro conhecido/ativo. O maior NodeID é uma prioridade determinística; o termo ajuda a distinguir rodadas, mas atualmente fica em memória.

## Funções e pontos de integração

- `superpeer.c`: recebe mensagens de rede C4 e encaminha ao controlador; a mudança de membership para `FAILED` alimenta o gatilho de eleição.
- `c4_cluster_tick`: avança a máquina de estados, inicia eleição e registra o evento `C4 ELECTION` no log do processo.
- `c4_cluster_handle`: processa `M_ELECTION`, `M_OK`, `M_COORDINATOR`, `M_SNAPSHOT` e `M_STATE_TRANSFER` recebidos.
- rotina de eleição no `c4_cluster.c`: enumera Super Peers ativos, compara NodeIDs, envia solicitações e aguarda respostas por timeout.
- rotina que assume a coordenação: consulta estado faltante antes de definir o nó como líder e divulgar o novo coordenador.

Esses pontos são partes de uma máquina integrada. Os nomes exatos dos helpers internos podem mudar; o contrato observável é o conjunto de mensagens, transições de estado e recuperação descritos acima.

## Como deve funcionar e o que foi demonstrado

O comportamento esperado é que, após a detecção de falha do líder, os sobreviventes escolham o maior NodeID ativo, divulguem o novo coordenador e permitam novas operações. O novo líder deve recuperar as entradas ausentes disponíveis nas réplicas sobreviventes antes de atender novas operações.

A integração TCP C4 documentada para `bash runc4.sh` passou build estrito com `-Werror`, teste local do log e 11 verificações TCP com três Super Peers e um Peer. O cenário observou a prioridade pelo maior NodeID, ANNOUNCE replicado, encerramento do líder, detecção `FAILED`, reeleição, preservação dos logs sobreviventes e uma nova operação no coordenador eleito. Isso demonstra o cenário de falha por parada testado; não comprova tolerância a partições ou consenso forte.

`tests/c4/run.sh` também passou 4/4 verificações após recompilação, mas suas chamadas a `put` e `state` são rejeitadas pela CLI e seus erros são ignorados. Por isso, o PASS genérico de SMR/log desse roteiro não é evidência de PUT replicado. O script foi mantido sem alteração.

## Limites e trabalho futuro

- A cobertura de eleição usa três Super Peers e falha por parada. Partições de rede e grupos maiores com atrasos não foram validados.
- O termo da eleição não é persistido; um reinício pode perder o histórico da rodada.
- A eleição e a replicação não formam consenso distribuído com quórum. O líder pode aplicar localmente uma operação antes de receber todas as confirmações.
- Metas de disponibilidade de 99,5%, recuperação abaixo de cinco segundos em geral e consistência forte não foram demonstradas.
- 2PC (`PREPARE`/`COMMIT`/`ABORT`) e IST pertencem ao Checkpoint 5 e permanecem pendentes.

## Comandos de execução

Na raiz do repositório, a integração C4 específica é executada com:

```bash
bash runc4.sh
```

O roteiro de testes C4 disponível em `tests/c4` pode ser executado com:

```bash
bash tests/c4/run.sh
```

Os resultados dos dois roteiros devem ser lidos separadamente, considerando as limitações de cobertura descritas acima.
