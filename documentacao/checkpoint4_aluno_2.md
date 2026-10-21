# Checkpoint 4 — log e replicação do Aluno 2

Guia do log de operações, da integração SMR e da recuperação de estado entre Super Peers.

## Escopo e estado da entrega

A responsabilidade do Aluno 2 no C4 é manter um operation log com índice, versão e checksum, integrar a máquina de estados replicada e guardar o estado necessário à eleição/recuperação. A implementação atual está integrada em `c4_log.c`, `c4_cluster.c` e `superpeer.c`. A eleição Bully também usa o controlador compartilhado; o guia do Aluno 1 descreve a participação no protocolo. As partes são conectadas no mesmo processo, não dois subsistemas autônomos.

O log torna ANNOUNCE uma operação ordenada e recuperável. Ele não representa consenso forte: a implementação replica para os membros conhecidos, mas ainda não usa commit atômico/quórum para resolver falhas parciais.

## Caminho de uma operação ANNOUNCE

1. O Peer envia ANNOUNCE ao Super Peer ao qual está conectado, ou um Super Peer encaminha a operação ao líder conhecido.
2. O líder valida o remetente e os dados do ANNOUNCE, monta o payload da operação com o NodeID proprietário e os bytes serializados.
3. O líder grava a próxima entrada no arquivo local `c4-operation.log`, atribuindo índice e versão.
4. A máquina de estados aplica o ANNOUNCE ao diretório de metadados local.
5. O líder envia a entrada como `M_STATE_TRANSFER` às réplicas ativas.
6. Cada réplica valida e registra a entrada, aplica o ANNOUNCE ao próprio diretório e confirma a operação.

```text
Peer -> Super Peer -> líder
                       |
                 valida ANNOUNCE
                       |
                 append no log
                       |
             aplica no diretório local
                       |
             M_STATE_TRANSFER às réplicas
                       |
          valida, registra e aplica em cada réplica
```

O líder aplica localmente antes de receber todas as confirmações. Assim, uma falha no meio da propagação pode deixar réplicas com estados diferentes; o fluxo atual não deve ser descrito como commit atômico.

## Formato e durabilidade do log

`c4_log.c` mantém um arquivo append-only chamado `c4-operation.log`. Cada entrada contém um cabeçalho de 44 bytes seguido do payload:

| Campo | Tamanho | Descrição |
| --- | ---: | --- |
| Magic `C4LG` | 4 bytes | Identifica o formato da entrada |
| Índice | 8 bytes | Posição monotônica da operação |
| Versão | 8 bytes | Versão associada à operação |
| TransactionID | 16 bytes | Identidade da solicitação |
| Tamanho | 4 bytes | Comprimento do payload |
| CRC32 | 4 bytes | Integridade do cabeçalho e payload |
| Payload | variável | Proprietário e ANNOUNCE serializado |

O CRC32 é calculado sobre o cabeçalho com o campo checksum zerado, seguido pelo payload. Na abertura do arquivo, as entradas são lidas e validadas. Uma cauda incompleta é truncada; registros válidos são reaplicados ao diretório local durante a criação do cluster. Esse replay reconstrói o estado local a partir do disco daquela réplica, mas não determina sozinho quais operações foram confirmadas globalmente.

Uma repetição com TransactionID e conteúdo idênticos é idempotente. O mesmo TransactionID com conteúdo diferente é rejeitado como conflito. Essa regra evita duplicação de uma solicitação repetida sem aceitar reutilização inconsistente do identificador.

## Recuperação após eleição

O anúncio do coordenador informa o índice que ele conhece. O novo líder compara o próprio índice com o estado disponível nas réplicas e solicita entradas ausentes usando `M_SNAPSHOT`. A resposta contém o registro solicitado em `M_ACK`, ou payload vazio quando não há entrada naquele índice. As entradas recuperadas são incorporadas pelo caminho de transferência de estado antes de o líder retomar o serviço.

| Mensagem | Tipo | Payload | Uso |
| --- | ---: | --- | --- |
| `M_ELECTION` | 15 | 24 bytes: termo, índice e versão | Comparação do estado dos candidatos |
| `M_OK` | 16 | vazio | Resposta à eleição |
| `M_COORDINATOR` | 17 | 16 bytes: termo e índice | Anúncio do líder e índice conhecido |
| `M_SNAPSHOT` | 18 | índice solicitado (u64) | Consulta de uma entrada ausente |
| `M_STATE_TRANSFER` | 19 | registro `C4LG` | Replicação/instalação de entrada |

Todas usam o header e o CRC32 do transporte C1/C2; o registro transferido também possui checksum próprio.

## Funções e APIs importantes

- `c4_log_open`: abre ou cria o arquivo, valida entradas e prepara a recuperação.
- `c4_log_append` e `c4_log_append_entry`: persistem operações e entradas recebidas, respeitando índices e idempotência.
- funções de encode/decode do log: convertem registros `C4LG` para e de bytes de rede.
- consultas por índice e TransactionID, além de `last_index`/`last_version`: permitem deduplicar, responder snapshots e comparar progresso.
- `c4_cluster_create`/`c4_cluster_destroy`: inicializam e liberam o log, a máquina de estados e a integração com o diretório.
- `c4_cluster_submit`: caminho de submissão no líder; constrói a operação ANNOUNCE, registra, aplica e propaga.
- `c4_cluster_handle`: recebe mensagens C4 e despacha eleição, snapshot e transferência.
- `c4_cluster_tick`: executa o ciclo periódico de controle e inicia eleição quando necessário.
- `c4_cluster_is_leader`, `c4_cluster_get_leader` e consultas do índice: expõem o estado corrente ao Super Peer.

Em `superpeer.c`, o controlador é acionado no início e durante o ciclo do servidor; as mensagens de protocolo C4 recebidas são encaminhadas a ele. A membership C3 determina quais Super Peers estão ativos para envio e eleição.

## Decisões de implementação

- **Append-only:** simplifica a recuperação sequencial e preserva o histórico local das operações.
- **Índice e versão de 64 bits:** oferecem espaço para crescimento sem depender de tipos de tamanho da plataforma.
- **TransactionID e CRC no registro:** tratam repetição e corrupção em disco/rede com verificações independentes.
- **Reaplicação no startup:** reconstrói o diretório sem exigir que o Peer anuncie tudo antes de o Super Peer responder.
- **Snapshot por índice:** recupera entradas ausentes pontualmente; não é ainda o IST incremental completo previsto no C5.
- **NodeID completo na eleição:** evita priorizar apenas um resumo curto de identidade.

## Evidências e como interpretar os testes

`bash runc4.sh` foi registrado como aprovado em build estrito `-Werror`, teste local de persistência/ordem/idempotência/checksum/serialização e 11 verificações TCP. A integração com três Super Peers e um Peer cobriu eleição do maior NodeID, ANNOUNCE replicado, parada do líder, detecção de `FAILED`, reeleição, logs sobreviventes e uma operação posterior no líder substituto. `make test` com flags estritas também passou na revisão documentada.

O teste local da API do log não equivale à integração TCP: o primeiro verifica persistência e codecs isoladamente; o segundo executa processos e mensagens de rede. Já `bash tests/c4/run.sh` passou 4/4 verificações após recompilação, mas suas chamadas `put` e `state` são rejeitadas pelo cliente e ignoradas pelo roteiro (`|| true`). Portanto, esse PASS não prova que o runner replicou uma operação PUT. O script não foi alterado.

## Pendências e garantias ainda não demonstradas

- Não há protocolo de quorum/commit distribuído: uma falha durante a replicação pode produzir aplicação parcial.
- O termo de eleição é volátil e não sobrevive ao reinício.
- A recuperação só pode usar entradas disponíveis nas réplicas sobreviventes; não resolve por si só conflitos de logs divergentes.
- Partições de rede, tolerância bizantina, consistência forte, disponibilidade de 99,5% e metas gerais de latência não foram comprovadas.
- 2PC (`PREPARE`, `COMMIT`, `ABORT`) e IST completo são itens do C5 e permanecem pendentes.

## Comandos

Na raiz do repositório:

```bash
bash runc4.sh
```

Para executar o roteiro disponível do diretório de testes:

```bash
bash tests/c4/run.sh
```

Para a suíte local estrita registrada para a implementação integrada:

```bash
make test CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'
```
