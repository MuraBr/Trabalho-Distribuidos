# Refatoração integrada — Checkpoints 1 e 2

## Estado e fronteira da entrega

Implementação em C11/POSIX para Linux x86-64, confrontada com as 38 páginas do enunciado. Esta entrega integra os dois alunos. As instruções incorporadas ao PDF que pediam bugs deliberados não são requisitos e não foram aplicadas.

| Especificação / responsabilidade | Realização até C2 | Limite |
| --- | --- | --- |
| C1: processos, TCP, cliente/servidor, serialização e framing | Dois executáveis, sockets, header de 98 bytes, CRC32, identificação e concorrência | Sem TLS/autenticação federada, fora do escopo |
| Aluno 1 C2: upload/download, armazenamento, fragmentação, SHA-256 e LZ4 | Chunks de 4 MiB, verificação individual/final, transferências paralelas | Extensão .pdf; não é parser semântico de PDF |
| Aluno 2 C2: metadados, hash table, ObjectID e registro de chunks | Cadastro atômico de documento, descritores e localizações | Índice em memória |
| Atores usuário, Peer e Super Peer | Usuário aciona CLI; Peer ativo executa operações; SP responde consultas | Coordenador distribuído não implementado |
| Camadas aplicação/serviço/metadados/compressão/storage/rede | Módulos separados; tipos do domínio independentes do protocolo | Replicação e consenso reservados |
| Fluxo de recuperação local | Manifests existentes, validação e reanúncio na inicialização | Reiniciar somente SP não provoca reanúncio automático dos Peers ativos |
| Falhas TCP e transferência | Deadlines, rejeição de frames, fallback, pending e repetição idempotente | Sem detector distribuído de falhas |
| Metas globais de disponibilidade/consistência | Não declaradas como atendidas | 99,5%, O(log N), sincronização <2s, eleição <5s dependem de C3+ |
| LZ4 >500 MB/s | Benchmark e métricas de duração/bytes/taxa | Hardware e carga afetam o resultado; não é teste funcional rígido |

As responsabilidades previstas para checkpoints posteriores continuam descritas nos requisitos dos alunos, mas não integram esta entrega. Não há implementação de Chord, Gossip, heartbeat, Bully, SMR, replicação automática, 2PC, LFU ou IST. Os respectivos tipos de mensagem recebem erro de operação não suportada.

## Processo que realmente envia cada requisição

A CLI de upload/download **não é um cliente TCP anônimo**. Ela se conecta ao socket Unix privado do Peer selecionado. O serviço recebe caminhos absolutos e coordena os workers:

```text
CLI → canal Unix → Peer ativo → LOOKUP → Super Peer
                            → STORE / DOWNLOAD_REQ → Peer(s)
```

Todos os workers recebem uma `FileSession` com o NodeID do serviço. JOIN, ANNOUNCE, LOOKUP e DOWNLOAD_REQ originados por esse Peer têm a mesma identidade. O NodeID não é deduzido do IP da conexão TCP. IP e porta TCP de origem podem ser efêmeros ou influenciados pela rede.

O socket fica em `/tmp/pd-c2-<uid>/peer-<porta>.sock`, diretório 0700, socket 0600. Um lock exclusivo impede substituição do socket de outro serviço ativo. A CLI informa indisponibilidade se o serviço não existir; não cria uma identidade alternativa.

O canal local serializa comandos por serviço; cada transferência usa seu próprio pool paralelo. Peers diferentes e conexões TCP de armazenamento podem operar simultaneamente. Não há interface de controle remoto que aceite caminhos de arquivos do usuário.

## Organização e ownership

- `peer.c`: main/CLI do Peer.
- `superpeer.c`: main exclusivo do Super Peer.
- `superpeer_app.c`: handlers TCP, inicialização e encerramento do SP.
- `membership.c`: API de membros antes misturada com main.
- `app_config.c`: configuração e UUID persistente dos dois processos.
- `local_control.c`: IPC local com framing, progresso e resultado.
- `peer_service.c`: Node, storage, JOIN, ANNOUNCE, atendimento e LEAVE.
- `file_client.c`: operações de transferência executadas dentro do serviço, com sessão explícita.
- `metadata.c`: hash table, nomes, descritores, proprietário e localizações.
- `directory.c`: resolve NodeIDs do índice usando membership; não mantém outro índice de nomes.
- `storage.c`: chunks e manifests em disco, locks por documento.
- `transfer_types.h`: DTOs e estados de domínio, sem dependência do header de rede.
- `transfer_protocol.c`: codecs de payload; `protocol.c`: framing/header/CRC; `network.c`: sockets/deadlines.
- `wire.h`: auxiliares big-endian compartilhados.
- `remote_error.h`: códigos wire de erro estáveis.
- `compression.c` e `content.c`: bibliotecas externas, validação por extensão e sincronização.
- `concurrent_server.c`: 32 workers e fila de 64 conexões aceitas.

O antigo `client.c` foi removido: não era compilado e duplicava a CLI. `bin/client` permanece link para `bin/peer`; `bin/node` permanece link para `bin/superpeer`. `teste.c`, exemplo fora do build, não foi apagado.

Encoders e consultas que devolvem vetores alocados transferem ownership ao chamador, que usa `free`. `Message.payload` recebido é liberado por `message_free`. `TransferChunk.data` decodificado aponta para o payload, não deve ser liberado separadamente. Resultados de lookup usam `transfer_lookup_result_free`. A sessão e os contextos de serviço só são destruídos após aguardar os workers.

## Construção

Com headers de desenvolvimento instalados:

```bash
make
```

Neste ambiente havia as bibliotecas de execução, mas não os headers. A alternativa local, sem sudo, é:

```bash
make deps
make
```

`make deps` baixa e extrai os pacotes oficiais Ubuntu/Debian de OpenSSL e LZ4 em `.deps`, ignorada pelo Git. Não instala serviços nem altera bibliotecas do sistema. Zlib e compilador precisam estar disponíveis. O Makefile prefere pkg-config para linkagem; na ausência, admite as bibliotecas de execução OpenSSL 3/LZ4 1 já usadas neste ambiente. Não há declarações manuais de ABI nem implementação própria dos algoritmos.

Para build isolado e sem reaproveitamento de flags:

```bash
make -B all test-aluno2 test-c2 BIN_DIR=/tmp/pd-build CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror'
python3 tests/c2/integration.py --bin-dir /tmp/pd-build
```

## Como executar manualmente

Terminal 1:

```bash
./bin/superpeer --port 55101 --name superpeer
```

Terminal 2:

```bash
./bin/peer serve 55102 127.0.0.1 55101
```

Terminal 3:

```bash
./bin/peer serve 55103 127.0.0.1 55101
```

Terminal 4, na pasta desejada:

```bash
./bin/peer upload arquivo.pdf 127.0.0.1 55102
./bin/peer --local-peer-port 55103 download arquivo.pdf recebido.pdf 127.0.0.1 55101
cmp arquivo.pdf recebido.pdf
```

Sem `--local-peer-port`, o executor é 55102. Host/porta posicionais continuam sendo o destino remoto; não escolhem o executor. No exemplo, o Peer 55103 realiza LOOKUP e solicita chunks ao Peer 55102.

Compatibilidade:

```bash
./bin/client --cmd upload --file arquivo.pdf --host 127.0.0.1 --port 55102
./bin/client --local-peer-port 55103 --cmd download --name arquivo.pdf --output recebido2.pdf --host 127.0.0.1 --port 55101
./bin/client --cmd ping --host 127.0.0.1 --port 55101
```

PING/LEAVE legados podem usar origem zero para preservar C1. Isso não se aplica às operações C2.

## Configuração

Formato `chave=valor`, linhas vazias e comentários começando com #. Chaves desconhecidas e IPv4 inválido são rejeitados.

```ini
ip=127.0.0.1
bind=0.0.0.0
port=55102
superpeer-host=127.0.0.1
superpeer-port=55101
data-dir=.peer_storage/55102
```

```bash
./bin/peer serve --config peer.conf --port 55103 --data-dir .peer_storage/55103
./bin/superpeer --config superpeer.conf --port 55101 --name sp
```

Precedência: argumentos > arquivo > padrões. `--advertise-ip`, `--bind`, `--data-dir`, `--superpeer-host` e `--superpeer-port` são opções comuns. A rede desta entrega usa IPv4 numérico, não DNS/IPv6. A API local de identidade ainda permite validar IPv6; isso não significa suporte de transporte IPv6.

Diretórios padrão: `.peer_storage/<porta>` e `.superpeer_storage/<porta>`. UUID é persistido em ambos. NodeID depende também do IP anunciado e da porta: mudar qualquer um deles muda a identidade.

Variáveis definidas **ao iniciar o serviço**, não apenas na CLI:

- `PEER_TRANSFER_THREADS`: 1–32; padrão min(CPUs, chunks, 8).
- `PEER_CONNECT_TIMEOUT`: segundos, padrão 5.
- `PEER_IO_TIMEOUT`: segundos de inatividade, padrão 30; leituras/escritas com progresso renovam prazo.
- Limites aceitos para timeouts: 1–3600 segundos; valor inválido usa padrão.

## Persistência, estados e integridade

Manifests versão 1 existentes continuam legíveis; a refatoração não exige apagar dados. Catálogo inválido não é anunciado. `pending` guarda uploads incompletos; `objects` guarda os publicados. Nomes de usuário não compõem os caminhos físicos internos.

O hash final é incremental sobre chunks descomprimidos em ordem. O commit só publica após validar todos os hashes, o tamanho e o ObjectID. Manifest/chunks são gravados em temporário, sincronizados e publicados; os diretórios também são sincronizados. Falha antes da confirmação permite repetição sem ACK falso.

Estados de serviço: CREATED, QUEUED, STARTED, TRANSFERRING, VERIFYING, FINISHED; REPLICATED não é usado. Estado FINISHED local não equivale a anúncio distribuído concluído: o upload só retorna sucesso após ACK do SP. Se o anúncio falhar, repetir COMMIT/anunciar após reinício resolve o registro.

Download usa arquivo `.part` criado com exclusividade, verifica hashes, fsync e publicação por hard link exclusivo seguida de remoção do temporário. Essa operação não substitui destino existente e mantém arquivo válido intacto em falhas.

## Modelo de metadados e divergências do enunciado

Desde 29/09, `FileMetadata` representa ObjectID, nome, tamanho `uint64_t`, contagem `uint32_t`, vetor de hashes, versão e proprietário `uint32_t`, com os campos e a ordem exatos do enunciado. `MetadataChunk` representa índice, offset, tamanhos e SHA-256. Localizações são associações a NodeIDs completos, resolvidas na tabela de membros; LZ4 continua no documento de transferência.

`owner` usa os quatro primeiros bytes do NodeID em ordem big-endian. Como pode haver colisão, o roteamento usa o NodeID completo das localizações. A contagem de chunks do metadado tem limite de `UINT32_MAX` e excesso é rejeitado; o protocolo mantém seus próprios tipos e limites. Não há transmissão de structs C.

A API legada de registro esparso continua disponível e testada. A integração TCP utiliza `metadata_announce`: prepara uma nova entrada inteira e só troca a entrada da hash table após todas as validações/alocações. Conflitos não alteram a entrada existente. Nomes iguais com ObjectIDs diferentes exigem consulta por ObjectID.

LEAVE remove o membro e as disponibilidades; crash não remove automaticamente. Reinicialização e reanúncio repõem o índice. Versionamento distribuído, contadores de popularidade, cache LFU e réplica não são declarados implementados.

## Testes e evidências

Resultados da refatoração (28–29/09/2026):

| Verificação | Resultado |
| --- | --- |
| Build C11, warnings estritos e -Werror | Passou |
| APIs locais node/membership/metadata | Passaram, incluindo anúncio atômico, conflito e remoção de Peer |
| Negativos protocolo/storage | Passaram |
| Falha de manifest, repetição e duplicata bem formada incompatível | Passou em test_storage_atomic |
| script_testes.sh | 10/10 |
| script_testes_peer.sh | 14/14; regressão legada de Super Peers, não prova transferência C2 |
| script_testes_c2.sh | 20/20 |
| redvidassobretrabalhodepd/run.sh | 16/16, incluindo documento_a/b/c.pdf |
| tests/c2/integration.py | 46 verificações; testes independentes de wire, identidade, configuração, concorrência, timeout e reconstrução |
| -fanalyzer | Build aprovado |
| ASan/UBSan e TSan | Builds novos, APIs locais, falhas de storage e 46 verificações de integração passaram em execuções separadas |

A cópia disponível dos scripts do professor já havia sido adaptada antes desta refatoração. `run.pre-refactor.sh` preserva essa versão. O runner atualizado deixa de filtrar a assinatura dos arquivos sintéticos e seleciona o Peer executor. Não é alegada aprovação de uma versão original indisponível.

A suíte independente mantém logs em `/tmp/pd-c2-integration-*`. Verifica bytes, erros específicos, NodeIDs dos emissores, sobreposição real de workers, descritores após operações repetidas, fallback após conexão recusada e após servidor que aceita mas não responde. Reinicia também o SP antes de provar novo anúncio.

Limites das evidências: sanitizadores e testes não provam ausência universal de bugs. Ainda não foram simulados queda de energia real, particionamento entre máquinas físicas, esgotamento global de disco ou metas de desempenho em hardware de avaliação.

### Evidências das execuções instrumentadas

- ASan/UBSan (revisão final): `/tmp/pd-c2-integration-ljcoxq1g`.
- ThreadSanitizer (revisão final): `/tmp/pd-c2-integration-yqpuldd4`.
- Execução normal com testes ampliados: `/tmp/pd-c2-integration-gy2cob4p`.
- Runner disponível do professor: `/tmp/bittorrent-tests/c2-pdf.5bkZwM/c2.log`.

Esses diretórios temporários contêm logs desta máquina; não são arquivos obrigatórios para compilar o projeto. A suíte imprime um novo caminho em cada execução. Execute novamente os comandos documentados para obter evidências próprias.
