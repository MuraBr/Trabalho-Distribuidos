# Checkpoint 2 — contrato de integração do aluno 2

Implementado em `metadata.c` / `metadata.h`. O módulo oferece uma API local em C, em memória e protegida por mutex. Não implementa mensagens TCP, upload, download, fragmentação, compressão ou persistência. Nenhum handler de `peer.c` chama esta API ainda.

## Ciclo de vida e ligação

O processo que mantém os índices do Super Peer deve criar um `MetadataStore` com `metadata_create` ao iniciar, compartilhá-lo entre seus handlers e destruí-lo com `metadata_destroy` depois de encerrar e aguardar as threads. Ele é independente da tabela de membros já existente em `superpeer.c`; não se deve criar um store novo a cada requisição.

Adicionar `metadata.c` à compilação do consumidor e manter `-pthread -Wl,-l:libcrypto.so.3` (ou `-lcrypto` onde disponível). Não há dependência de banco de dados. Como o módulo de identidade existente, este módulo usa a ABI de libcrypto disponível no ambiente; os tipos opacos e as declarações EVP estão limitados a `metadata.c`.

## Dados e convenções

- `ObjectID`: 32 bytes do SHA-256 de **todo o arquivo original**, antes da fragmentação/compressão. Não é o NodeID, nem o hash de um chunk.
- `object_id_file`: leitura incremental em buffers de 64 KiB; retorna ObjectID e tamanho original. O chamador deve garantir que o arquivo não mude durante a leitura. Arquivo vazio tem o SHA-256 padrão do conteúdo vazio.
- `MetadataDocument`: ObjectID, primeiro nome registrado (1 a 255 bytes), tamanho original e quantidade de chunks.
- Convenção adotada para os “4 MB” do enunciado: `METADATA_CHUNK_SIZE = 4194304` bytes (4 MiB). A fragmentação do aluno 1 deve usar essa constante; último chunk pode ser menor.
- Índices começam em zero; quantidade calculada por teto(tamanho / tamanho do chunk), sem overflow. Arquivo vazio tem zero chunks.
- Associação: `(ObjectID, índice do chunk, NodeID do peer)`. Um chunk pode ter vários peers; registrar a mesma associação novamente não a duplica.
- Documento repetido com mesmo ID e tamanho é idempotente. Outro nome preserva o primeiro; tamanho divergente retorna `EEXIST`, sem alterar o registro.
- O registro de metadados recebe declarações do chamador: não comprova que um peer possui o conteúdo ou que o ID recebido corresponde a ele. O handler deve validar a identidade e a autorização de quem anuncia os chunks.

## Sequência de integração

1. No lado que lê o arquivo, chamar `object_id_file` antes da fragmentação. Reutilizar esse identificador no upload e na validação do download.
2. No índice do Super Peer, chamar `metadata_register_document` com ObjectID, nome e tamanho original recebidos e validados.
3. Após confirmar que um peer armazena um chunk, chamar `metadata_register_chunk`. Registrar documento não anuncia automaticamente todos os chunks.
4. Para consultar, chamar `metadata_find_document` e `metadata_chunk_peers` para cada índice necessário.
5. Resolver cada NodeID retornado com `superpeer_find_member` para obter IP e porta. A API de metadados não exige membership: o handler deve validar o peer antes do anúncio e tratar peers que já saíram.
6. Ao perder/remover uma cópia, chamar `metadata_unregister_chunk`. `metadata_remove_document` remove o índice inteiro e todas as associações, sem apagar arquivos físicos.

Ao sair um peer, o consumidor deve retirar seus anúncios conhecidos; não há limpeza automática por LEAVE ou detecção de falha nesta entrega. Não há snapshot conjunto entre as tabelas de membros e metadados.

## Retornos e memória

As funções que retornam `int` usam `0` para sucesso e `-1` para erro, com `errno`. Principais erros: `EINVAL` para parâmetros/índices inválidos, `ENOENT` para documento/associação ausente, `EEXIST` para tamanho incompatível, `ENOMEM` para falta de memória. A leitura do arquivo também pode retornar erros do sistema.

`metadata_find_document` retorna uma cópia. `metadata_chunk_peers` retorna um vetor independente, que o chamador libera com `free()`. Ausência de peers para um índice válido é sucesso com `NULL` e contagem zero. Saídas são preservadas em erro, exceto `metadata_create`, que inicializa seu ponteiro de saída com `NULL`. A ordem dos peers não é definida.

Não transmitir structs ou ponteiros pela rede. O aluno 1 deve definir a serialização dos campos, tamanhos, limites, ordem dos bytes e validação dos payloads; ObjectID e NodeID são bytes binários, não strings. O formato wire de STORE/LOOKUP ainda não está implementado ou fixado por esta API.

## Estrutura e limites

Hash table de 257 buckets, colisões por encadeamento e comparação dos 32 bytes completos. A busca percorre apenas o bucket correspondente, mas a tabela não redimensiona: não há garantia de tempo constante para crescimento ilimitado. As disponibilidades são listas esparsas por documento; consultas de chunks percorrem essas listas. Um mutex por store protege operações e cópias de saída. Isso fornece consistência local; não implementa consenso ou consistência distribuída.

Não há persistência, Chord, Gossip, replicação, 2PC, validação do formato PDF ou hash individual de chunks neste módulo.

## Validação executada em 24/09/2026

- `make test-aluno2`: C1 local de node/superpeer e C2 de metadados aprovados.
- `make test`: as duas suítes acima e a suíte existente de protocolo aprovadas. Na última execução dentro do sandbox, o envio por socket foi bloqueado; a repetição fora do sandbox passou.
- C2 compilado com `-fsanitize=address,undefined -g`: aprovado fora do sandbox, incluindo verificação de vazamentos; dentro do sandbox o LeakSanitizer não conseguiu executar por restrição do ambiente.
- Casos C2: hashes SHA-256 conhecidos (vazio, abc, arquivo de 200000 bytes), arquivo ausente, duplicatas, aliases, conflito de tamanho, índices inválidos, documento desconhecido, múltiplos peers, remoção, saída independente, oito threads, 600 documentos com colisões garantidas, fronteiras zero/4 MiB/4 MiB+1/UINT64_MAX argumentos inválidos e preservação das saídas em erro.
- Não executados: integração C2 via rede (ainda inexistente), testes com processos distribuídos, ThreadSanitizer e injeção de falhas de alocação. O teste concorrente não prova ausência de todas as condições de corrida.
