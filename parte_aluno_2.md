# Parte do aluno 2 — identidade, Super Peer e metadados

Este documento explica a implementação atual dos checkpoints 1 e 2 atribuída ao aluno 2. Para o estado detalhado de cada requisito, consulte [requisitos_aluno_2.md](requisitos_aluno_2.md).

## Visão geral

O aluno 2 cuida da identidade dos nós, do cadastro de membros no Super Peer e do índice de documentos. O Super Peer sabe **qual documento existe e quais Peers possuem seus chunks**. Os bytes do PDF são armazenados e transferidos pelos Peers; essa parte é integrada ao trabalho do aluno 1.

| Arquivo | Responsabilidade |
| --- | --- |
| `node.c` e `node.h` | Validar IP/porta/UUID e calcular o NodeID com SHA-256. |
| `superpeer.c` e `superpeer.h` | Cadastrar, consultar, atualizar e remover membros do Super Peer; atender as mensagens TCP e iniciar o programa. |
| `metadata.c` e `metadata.h` | Calcular ObjectID, manter a hash table de arquivos e registrar disponibilidade de chunks. |
| `directory.c` | Converter mensagens de anúncio e consulta em operações da API local de metadados e membros. |

## 1. Identidade do nó

`NodeConfig` contém IP, porta e UUID. `node_compute_id` calcula um NodeID de 32 bytes aplicando SHA-256 ao endereço IP em formato binário, à porta em ordem de rede e ao UUID. O PID fica no `Node`, mas não entra nesse hash: ele identifica apenas o processo local.

No JOIN, o Super Peer reconstrói a identidade a partir dos dados recebidos e compara o NodeID calculado ao informado na mensagem. Isso detecta uma identidade inconsistente; não equivale a autenticação criptográfica do Peer. Os processos persistem o UUID para manter o NodeID ao reiniciar com a mesma configuração.

## 2. Cadastro de membros

`SuperPeer` começa com seu próprio nó cadastrado. `superpeer.c` guarda os membros em um vetor que cresce conforme necessário e protege as operações com um mutex. Cada registro contém o `Node`, o estado `ALIVE` e o instante `last_seen`.

Ao registrar um NodeID novo, a tabela acrescenta um membro. Ao registrar novamente o mesmo NodeID, atualiza o cadastro sem aumentar a contagem. Uma saída voluntária por LEAVE remove o membro e, na integração, chama `metadata_remove_peer` para retirar suas localizações de chunks. O cadastro usa busca linear; não há Chord, Gossip ou detector automático de falhas nesta etapa.

## 3. Estrutura dos metadados

`metadata.h` declara a estrutura solicitada no enunciado, com os campos nesta ordem:

```c
typedef struct {
    uint8_t object_id[32];
    char filename[256];
    uint64_t size;
    uint32_t chunk_count;
    uint8_t **chunk_hashes;
    uint32_t version;
    uint32_t owner;
} FileMetadata;
```

- `object_id`: SHA-256 dos bytes completos do arquivo, calculado pela API `object_id_file` com OpenSSL EVP e leitura incremental.
- `filename` e `size`: nome e tamanho original do PDF.
- `chunk_count`: quantidade de blocos de até 4 MiB; um arquivo que exigiria mais de `UINT32_MAX` chunks é rejeitado.
- `chunk_hashes`: vetor de hashes SHA-256 dos chunks. Um cadastro esparso ainda não anunciado pode ter esse campo nulo.
- `version`: começa em 1; não há versionamento distribuído.
- `owner`: valor de 32 bits formado pelos quatro primeiros bytes do NodeID do anunciante, em ordem big-endian. É um resumo sujeito a colisões. A localização e o encaminhamento usam o NodeID completo, guardado separadamente.

`FileMetadata` é uma estrutura em memória, **não** o formato transmitido na rede. LZ4 e os tamanhos comprimidos pertencem ao documento e aos descritores de transferência; não são campos dessa estrutura.

## 4. Hash table e registro dos chunks

`MetadataStore` usa 257 buckets. Um hash escolhe o bucket e a comparação dos 32 bytes do ObjectID distingue documentos mesmo quando há colisão de bucket. O mutex protege as operações locais entre threads. O índice fica em memória no Super Peer.

`metadata_register_document` e `metadata_register_chunk` formam a API local de cadastro esparso: é possível registrar um documento sem alocar um vetor enorme de chunks e associar Peers a índices específicos. `metadata_announce` é o caminho da integração TCP: valida todos os descritores, prepara o novo registro e só então o publica na tabela. Se houver conflito de tamanho ou hash, o registro anterior permanece.

Cada `MetadataChunk` contém índice, posição no arquivo, tamanho original, tamanho comprimido e hash. A disponibilidade associa **índice do chunk + NodeID completo do Peer**. Um mesmo chunk pode ter mais de uma localização. `metadata_chunk_peers` devolve uma cópia da lista de Peers, que o chamador libera com `free`.

`metadata_find_document` devolve uma cópia independente de `FileMetadata` e dos hashes. Depois de usá-la, o chamador chama `file_metadata_free`. `metadata_find_name` procura pelo nome e acusa ambiguidade quando ObjectIDs diferentes compartilham esse nome.

## 5. Integração com upload e download

1. O Peer entra na rede com JOIN; o Super Peer valida sua identidade e o cadastra.
2. No upload, o Peer calcula o ObjectID, divide o PDF em chunks, calcula os hashes e comprime os chunks com LZ4.
3. Após confirmar o armazenamento, o Peer envia ANNOUNCE. `superpeer.c` valida o pedido e `directory.c` adapta os dados para `FileMetadata` e `MetadataChunk`.
4. No download, LOOKUP consulta o índice por nome ou ObjectID. `directory.c` combina descritores de chunks com os NodeIDs disponíveis e consulta a tabela de membros para obter IP e porta.
5. O Peer que baixa solicita os chunks diretamente aos Peers indicados. O arquivo final é verificado pelo SHA-256 do ObjectID antes de concluir o download.

O programa mostra, no upload, `File`, `Size`, `ObjectID`, `Chunks`, os hashes de cada chunk e `Compression: LZ4`. Após o download bem-sucedido, mostra `Download completed` e `SHA-256 verified`. Essas mensagens são produzidas pelo fluxo de transferência integrado, não somente pela API local do aluno 2.

## 6. Como demonstrar

Na raiz do projeto, compile e execute os testes:

```bash
make -B all
make test-aluno2
make test-c2
python3 tests/c2/integration.py --bin-dir bin
bash script_testes_c2.sh
```

Use `make all` explicitamente, pois o alvo padrão do Makefile é `deps`. O script C2 usa, por padrão, as portas 55101, 55102 e 55103. Na verificação da versão de 29/09/2026, os testes locais e C2 passaram, a integração TCP registrou 46 verificações aprovadas e `script_testes_c2.sh` terminou com 20/20.

Para uma demonstração manual, use terminais separados para os três primeiros comandos e, depois, execute o upload e o download:

```bash
./bin/superpeer --port 55101 --name superpeer
```

```bash
./bin/peer serve 55102 127.0.0.1 55101
```

```bash
./bin/peer serve 55103 127.0.0.1 55101
```

```bash
./bin/peer upload arquivo.pdf 127.0.0.1 55102
./bin/peer --local-peer-port 55103 download arquivo.pdf recebido.pdf 127.0.0.1 55101
```

O segundo comando de transferência usa o Peer ativo da porta 55103. O resultado depende de existir um `arquivo.pdf` de teste e de as portas estarem livres.

## 7. Limites desta entrega

- O índice de metadados do Super Peer é volátil. Após reiniciar Super Peer e Peers, os Peers reanunciam os arquivos; reiniciar somente o Super Peer não solicita automaticamente anúncios dos Peers já ativos.
- `ALIVE` e `last_seen` não significam heartbeat ou detecção automática de falhas. LEAVE cobre a saída voluntária.
- O fluxo de CRC inválido fecha a conexão sem enviar a resposta ERROR exigida em uma parte do requisito.
- Replicação consistente, eleição, SMR, 2PC e IST pertencem aos checkpoints posteriores e não estão implementados nesta entrega.
