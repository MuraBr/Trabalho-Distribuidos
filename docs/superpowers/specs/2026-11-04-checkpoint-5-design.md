# Desenho do Checkpoint 5 — cache, 2PC e IST

## Intenção

Concluir o Checkpoint 5 para os dois alunos, conectando cache LFU ao caminho real de transferência de chunks do Peer e usando 2PC e Incremental State Transfer (IST) no fluxo de anúncios de metadados entre Super Peers.

## Requisitos e resultado esperado

- **Aluno 1:** manter cache LFU limitado por bytes no armazenamento do Peer; registrar cache hit, cache miss, frequência e eviction; servir chunks de upload/download pelo cache quando presentes.
- **Aluno 2:** executar PREPARE, COMMIT e ABORT para a publicação de ANNOUNCE em todos os Super Peers ativos; persistir estado preparado/decisão para idempotência e recuperação local; sincronizar entradas ausentes em lotes incrementais ordenados por índice/versão.
- O runner C5 deve iniciar os processos necessários e validar operações reais, além de cobrir resultado de commit, abort, cache hit/miss/eviction e recuperação incremental.
- Preservar framing C1/C2 e os tipos de mensagem existentes. Manter leitura dos logs C4 atuais sem migração destrutiva.

## Fora de escopo e limites

- Não alterar `requisitos_aluno_1.md`.
- Não declarar tolerância a partições, consenso de quórum, disponibilidade de 99,5% ou progresso do 2PC após perda permanente do coordenador. O 2PC clássico pode bloquear enquanto uma decisão fica em dúvida.
- O cache é volátil e não substitui o armazenamento durável dos chunks. Eviction remove apenas a cópia em memória; o Peer ainda pode reler o chunk persistido.
- Não mudar a semântica C2 de upload, ObjectID, SHA-256 ou compressão LZ4.

## Abordagens consideradas

1. **Integrar nos módulos existentes (escolhida):** cache no `Storage` usado por `peer.c`; fases transacionais e IST no cluster/log C4. Reutiliza os pontos reais de leitura de chunks, ANNOUNCE, log e membership, evitando uma segunda implementação de armazenamento/protocolo.
2. **Criar módulos C5 separados:** separaria cache, transações e IST, mas exigiria novas camadas de adaptação ao armazenamento e ao `C4Cluster`, duplicando parte dos contratos que já existem.

Foi escolhida a primeira abordagem. O usuário aprovou essa opção em 04/11/2026.

## Desenho proposto

### Cache LFU — Aluno 1

`Storage` manterá uma cache concorrente de chunks comprimidos, indexada por ObjectID e índice do chunk. `storage_read_chunk` consultará a cache antes do arquivo persistido. Uma leitura em disco será miss e preencherá a cache se houver capacidade; uma leitura encontrada será hit e incrementará a frequência saturável. Quando faltar capacidade, serão removidos os chunks de menor frequência; empates serão resolvidos pela entrada menos recentemente acessada. Chunks maiores que a capacidade não serão armazenados na cache. O default proposto é 16 MiB, configurável por `PEER_CACHE_BYTES`; zero desativa a cache. A API de snapshot de estatísticas permitirá testes sem depender de texto de log.

O protocolo de download existente já solicita os chunks ao Peer e recebe o conteúdo validado. Os testes usarão esse caminho para exercitar a cache do Peer servidor: primeira leitura produz miss, repetição produz hit, e leitura de chunks distintos acima da capacidade produz eviction. Logs concisos do Peer permitirão inspecionar o comportamento durante a integração.

### 2PC — Aluno 2

O limite transacional é um ANNOUNCE C2 já validado, antes de tornar seus metadados visíveis nos catálogos Super Peer. O coordenador captura a lista de Super Peers ativos, grava localmente a preparação durável e envia `M_PREPARE` com entrada C4 serializada e TransactionID original. Cada participante valida líder, membership, checksum, índice/versão esperados e idempotência; armazena a entrada preparada sem aplicá-la ao diretório e responde voto positivo somente após persistência.

Se todos os participantes votarem positivamente, o coordenador persiste a decisão COMMIT antes de notificar participantes. Cada `M_COMMIT` move a entrada para o log committed e aplica ANNOUNCE uma única vez; respostas repetidas são idempotentes. Qualquer voto negativo ou timeout antes da decisão faz o coordenador persistir ABORT e notificar os participantes preparados; uma operação abortada nunca entra no log committed nem no diretório. O Peer recebe ACK apenas depois de concluído o caminho de commit exigido pelo conjunto capturado.

As decisões e preparações terão framing/versionamento/checksum próprios, em arquivos auxiliares do diretório C4, sem alterar o formato dos registros `C4LG` existentes. Os tipos `M_PREPARE`, `M_COMMIT` e `M_ABORT` já reservados serão usados; payloads serão validados por TransactionID, NodeID do líder e índice/versão.

### Incremental State Transfer — Aluno 2

Serão acrescentadas mensagens de solicitação e resposta IST versionadas para pedir entradas posteriores ao último índice/versão local. O líder devolverá lotes limitados por tamanho de payload, contendo registros `C4LG` completos e ordenados. O receptor validará checksums, continuidade de índice/versão e TransactionID, persistirá e aplicará cada lote, avançando seu cursor somente após sucesso. Solicitações seguintes buscarão o restante; payloads C4 atuais continuarão válidos.

Após eleição/ingresso, o coordenador recuperará entradas committed que lhe faltam de uma réplica ativa. Membros que aceitam um coordenador compararão o próprio índice e solicitarão IST a ele. Uma diferença de versão não será ocultada: lacunas, dados divergentes ou limite de lote inválido interrompem a sincronização com erro observável.

### Testes e documentação

Testes locais cobrirão LFU, saturação/tie-break, capacidade zero, persistência de chunks após eviction, idempotência/conflito de PREPARE/COMMIT/ABORT, crash/reabertura do journal e paginação/validação de IST. O runner `tests/c5/run.sh` será substituído por um cenário TCP autocontido com diretórios temporários, Peer e três Super Peers usando `tests/config/c5.conf`; deverá conferir bytes de download, eventos de protocolo e estados/logs replicados sem aceitar erros ignorados como sucesso. Build estrito e regressões C1–C4 relevantes serão executados separadamente.

`requisitos_aluno_2.md` será atualizado com estado real, evidências e limites; `explicacao_codigo_aluno_2.md` receberá a explicação do novo comportamento. Comentários nos arquivos C do Aluno 2 e testes correspondentes permanecerão em português.

## Decisões deixadas ao implementador

- Implementar o journal C5 auxiliar sem modificar a codificação C4 existente.
- Usar limite máximo do protocolo existente para formar lotes IST e garantir progresso mesmo para a maior entrada válida.
- Manter frequência e idade de acesso em contadores monotônicos internos; expor estatísticas por cópia protegida por mutex.
- Dar ao runner C5 diretórios exclusivos por execução e encerrar somente os processos que ele próprio iniciou.
