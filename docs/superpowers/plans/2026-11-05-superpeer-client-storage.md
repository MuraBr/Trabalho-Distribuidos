# Super Peer como servidor de clientes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fazer o `tests/c5/run.sh` original passar sem modificá-lo, usando Super Peers como endpoints de clientes com armazenamento durável e cache LFU locais.

**Architecture:** Reutilizar `Storage` no processo Super Peer para receber `M_STORE`, servir `M_DOWNLOAD_REQ` e aplicar cache LFU. O upload concluído gera ANNOUNCE submetido ao C4/2PC. A CLI encaminha upload, lookup por nome e download diretamente ao Super Peer; lookup aquece apenas o primeiro chunk. Eventos IST são registrados quando lotes reais são enviados/aplicados.

**Tech Stack:** C2x, POSIX sockets/pthreads, protocolo TCP existente, `Storage`, metadata/directory, C4 journal/cluster e scripts bash/Python existentes.

**Spec:** `docs/superpowers/specs/2026-11-05-superpeer-client-storage-design.md`

## Global Constraints

- Não modificar `tests/c5/run.sh` nem `tests/config/c5.conf`.
- Não transmitir structs C diretamente na rede; usar serializadores de `transfer_protocol` e protocolo existente.
- Validar origem/destino, limite de payload, framing, CRC, ObjectID, descritores e estado do documento.
- Não tornar ANNOUNCE visível antes de COMMIT durável.
- Reutilizar cache LFU do `Storage`; não imprimir eventos de teste sem uma operação correspondente.
- Manter comentários em português em `superpeer.c`; não alterar `requisitos_aluno_1.md`.
- `lookup --name` executa consulta e aquece somente o primeiro chunk via leitura real do armazenamento.

## Review Focus

- `M_STORE` ANNOUNCE legado não pode ser confundido com STORE_BEGIN/CHUNK/COMMIT; teste de cada operação e payload malformado.
- Cliente pode usar NodeID não registrado no membership; aceitar somente o protocolo de ingestão destinado ao SP e rejeitar mensagens com destino/origem inválidos.
- ANNOUNCE atribuído ao SP local precisa resolver sua localização no catálogo; testar lookup e download posterior.
- Lookup de nome ausente, vazio, longo ou ambíguo deve retornar erro previsível sem aquecer cache.
- Falha de armazenamento antes do commit não deve publicar metadata; recuperação/replay 2PC deve continuar íntegra.

## Arquivos a alterar

- `superpeer.c`: inicializar/destruir `Storage`, tratar STORE_BEGIN/CHUNK/COMMIT, servir downloads e publicar ANNOUNCE.
- `Makefile`: linkar fontes e dependências de Storage/Content/Compression no executável Super Peer.
- `peer.c` e `file_client.c`/`.h`: CLI de cliente direto, lookup de documento por nome e aquecimento do primeiro chunk.
- `c4_cluster.c`: logs de IST associados a solicitações/lotes realmente aplicados; logs PREPARE/COMMIT/ABORT sem prefixo `C5` permanecem.
- `tests/c5/transaction_integration.py` e novos casos de integração: validar storage do SP, nome, cache e manutenção do caminho existente de Peer.
- `requisitos_aluno_2.md`, `explicacao_codigo_aluno_2.md` e guia C5: refletir a função atual do Super Peer e evidências reais.

### Task 1: Super Peer persiste uploads recebidos diretamente

**Files:** `superpeer.c`, `Makefile`, `tests/c5/transaction_integration.py`.

- [x] Escrever cenário de integração para upload direto, lookup e download do Super Peer.
- [x] Executar cenário inicial e confirmar falha antes da implementação por ausência do serviço Peer local.
- [x] Adicionar `Storage *storage` ao contexto do Super Peer; criar armazenamento local e configurar cache com padrão de 16 MiB.
- [x] Tratar operações de transferência M_STORE sem confundi-las com ANNOUNCE C4.
- [x] Após commit durável, submeter ANNOUNCE do Super Peer ao C4 e responder após o fluxo de replicação.
- [x] Validar upload direto e recuperação de conteúdo pela integração TCP.

### Task 2: Super Peer serve chunks e cache

**Files:** `superpeer.c`, `Makefile`, `tests/c5/transaction_integration.py`.

- [x] Escrever integração que confirma MISS/HIT e download pelo Super Peer.
- [x] Implementar handler de download com cache LFU do armazenamento.
- [x] Validar pedidos e retornar erro quando o chunk não pode ser servido.
- [x] Executar integração e testes locais de LFU.

### Task 3: CLI conecta ao Super Peer e procura por nome

**Files:** `peer.c`, `file_client.c`, `file_client.h`, `tests/c5/transaction_integration.py`.

- [x] Adicionar integração para os comandos `upload --file` e `lookup --name` do runner.
- [x] Reutilizar cliente de arquivos para upload direto ao endpoint SP.
- [x] Implementar lookup de documento por nome e preservar lookup Chord por ObjectID.
- [x] Aquecer cache com leitura real somente do primeiro chunk durante lookup.
- [x] Baixar chunks diretamente do Super Peer mantendo o serviço Peer.
- [x] Executar os fluxos integrados e comparar os bytes do download.

### Task 4: Tornar a localização do próprio Super Peer válida no catálogo

**Files:** `directory.c`/`.h`, `metadata.c`/`.h`, `superpeer.c`, `tests/c5/transaction_integration.py`.

- [x] Confirmar upload do Super Peer seguido de lookup e download por sua localização.
- [x] Aceitar localização do Super Peer local no catálogo.
- [x] Validar recuperação de estado de Super Peer atrasado e igualdade dos logs C4.
- [x] Manter e executar o caminho integrado de Peer normal.

### Task 5: Mensagens de IST e runner original

**Files:** `c4_cluster.c`, `tests/c5/transaction_integration.py`, documentação C5.

- [x] Exigir no cenário integrado log de aplicação de lote IST real.
- [x] Registrar envio e aplicação de lotes IST com cursor e contagem.
- [x] Executar `bash tests/c5/run.sh` intacto: PASS 5, FAIL 0.
- [x] Executar `bash tests/c5/run_integrated.sh` com build estrito e cenários TCP.
- [x] Atualizar documentação Markdown do Aluno 2, mantendo `requisitos_aluno_1.md` intacto.

## Revisão final

- Confirmar que `git diff -- tests/c5/run.sh tests/config/c5.conf` está vazio.
- Confirmar que os testes usam dados e respostas reais; nenhuma saída de PASS depende apenas de strings emitidas no startup.
- Confirmar build estrito `-Werror`, integração de Peer e ambos runners.
