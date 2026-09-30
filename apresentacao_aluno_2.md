# Roteiro de apresentação — parte do aluno 2

Este roteiro explica o que está implementado, como os dados circulam e por que cada estrutura foi usada. Ele cobre a identidade e o cadastro do checkpoint 1 e, principalmente, os metadados do checkpoint 2. A explicação técnica mais curta está em [parte_aluno_2.md](parte_aluno_2.md); o acompanhamento dos requisitos está em [requisitos_aluno_2.md](requisitos_aluno_2.md).

## Abertura: o que eu implementei

> “Minha parte permite que os nós se identifiquem e que o Super Peer saiba onde encontrar cada documento. No primeiro checkpoint, implementei o NodeID e o cadastro de membros. No segundo, implementei o ObjectID, a estrutura de metadados pedida no enunciado, uma hash table de documentos e o registro dos Peers que possuem cada chunk. Também integrei esse índice aos anúncios e às consultas recebidos pela rede.”

O Super Peer mantém **informações sobre o PDF**, como tamanho, hashes e localizações. Os Peers guardam e transferem os bytes do arquivo. Essa separação permite consultar o índice antes de escolher de qual Peer baixar os chunks.

## 1. Identidade: como um Peer entra na rede

> “Cada nó tem IP, porta e UUID. Eu calculo um NodeID de 32 bytes com SHA-256 sobre o IP em formato binário, a porta em ordem de rede e o UUID. O PID é guardado para representar o processo local, mas não participa do identificador de rede. Quando um Peer envia JOIN, o Super Peer reconstrói esse NodeID a partir dos dados recebidos e só cadastra o Peer se ele corresponder ao NodeID informado.”

**Por que assim?** O IP e a porta indicam o endereço do serviço; o UUID distingue instâncias que usam o mesmo endereço em momentos diferentes. Usar o formato binário do IP evita que grafias textuais diferentes alterem o hash. O projeto usa OpenSSL para SHA-256, em vez de reimplementar criptografia. Persistir o UUID mantém o NodeID estável quando o processo reinicia com a mesma configuração.

**No código:** `node.c` valida a configuração e calcula a identidade; `superpeer.c` trata JOIN e cadastra o nó. A comparação identifica dados inconsistentes, mas **não autentica** quem enviou a mensagem.

## 2. Cadastro de membros: quem está disponível

> “O Super Peer começa com ele próprio na tabela. Quando recebe um NodeID novo, acrescenta um membro. Se o mesmo NodeID já existe, atualiza o registro sem duplicá-lo. Cada membro tem os dados do nó, estado ALIVE e o horário do último cadastro. Na saída voluntária por LEAVE, o membro e suas localizações de chunks são removidos.”

**Por que um vetor dinâmico e mutex?** O vetor é simples para a escala deste checkpoint e cresce quando necessário. O mutex evita que duas threads alterem ou leiam a tabela durante uma realocação. A busca é linear, então o projeto ainda não oferece a estrutura distribuída nem a complexidade de consulta esperada para checkpoints posteriores. `ALIVE` e `last_seen` representam o cadastro local; ainda não existe heartbeat que detecte um processo que caiu sem enviar LEAVE.

**No código:** `superpeer.c` contém a tabela e liga JOIN/LEAVE à rede. O teste local compila somente a parte de membros com `SUPERPEER_MEMBERSHIP_ONLY`.

## 3. ObjectID e metadado do arquivo

> “No segundo checkpoint, o identificador do documento é o SHA-256 dos bytes completos do arquivo, chamado ObjectID. Ele permite reconhecer o conteúdo independentemente do nome. O cálculo lê o arquivo em blocos de 64 KiB, sem carregar todo o PDF na memória.”

**Por que SHA-256 incremental?** O tamanho do arquivo não precisa caber na memória. A API EVP da OpenSSL fornece uma implementação pronta do algoritmo. O mesmo ObjectID também permite verificar o arquivo completo após o download.

A estrutura pública em `metadata.h` é exatamente a solicitada:

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

> “`object_id` identifica o conteúdo; `filename` e `size` descrevem o arquivo; `chunk_count` informa a divisão em blocos de até 4 MiB; `chunk_hashes` contém os hashes individuais; `version` começa em 1; e `owner` é um valor numérico de 32 bits derivado do NodeID do anunciante.”

**Por que guardar hashes por chunk?** Assim o Peer que baixa pode conferir cada bloco recebido antes de montar o arquivo e conferir o ObjectID final. Se um bloco chegar corrompido, o erro fica localizado naquele chunk.

**Observação para a banca:** `owner` tem apenas 32 bits porque esse é o tipo exigido pela estrutura. Ele contém os quatro primeiros bytes do NodeID e pode colidir. Para localizar Peers, o sistema guarda **o NodeID completo de 32 bytes** nas associações de disponibilidade; não usa `owner` como endereço de rede. LZ4 também não é campo de `FileMetadata`: a compressão fica no documento e nos descritores da transferência.

## 4. Hash table: como o Super Peer encontra documentos

> “Eu mantenho uma hash table indexada pelo ObjectID. O hash escolhe um dos 257 buckets; se dois ObjectIDs caírem no mesmo bucket, uma lista encadeada guarda ambos. A busca compara os 32 bytes completos, então uma colisão de bucket não faz um arquivo ser confundido com outro.”

**Por que essa estrutura?** Ela oferece um índice local simples para anúncios e consultas por ObjectID. O número fixo de buckets facilita a implementação e o teste de colisões, mas a tabela não redimensiona: com muitos documentos, as listas podem crescer e a busca ficar mais lenta. A procura por nome percorre o índice e retorna erro de ambiguidade se documentos diferentes tiverem o mesmo nome; usar o ObjectID resolve esse caso.

O índice é protegido por mutex porque o servidor atende conexões em paralelo. Ele fica em memória no Super Peer. Reiniciar apenas o Super Peer apaga o índice; quando os Peers também reiniciam e fazem novos anúncios, o índice é reconstruído.

## 5. Registro de chunks e publicação atômica

> “Cada chunk tem índice, posição no arquivo, tamanho original, tamanho comprimido e hash. Eu associo o índice e o ObjectID aos NodeIDs dos Peers que possuem aquele bloco. Um chunk pode ter mais de uma localização.”

Há dois caminhos na API de metadados:

1. **API local esparsa:** `metadata_register_document` cadastra nome, tamanho e quantidade de chunks sem alocar um vetor para todos eles; `metadata_register_chunk` adiciona localizações conforme necessário. Isso evita reservar memória proporcional a um arquivo enorme apenas para criar seu registro.
2. **Anúncio completo pela rede:** `metadata_announce` valida os descritores, os hashes e os tamanhos, prepara uma entrada nova e só a publica depois de concluir as alocações e verificações. Em conflito, mantém o registro anterior.

**Por que publicar tudo de uma vez?** Uma consulta concorrente deve ver o documento anterior ou o documento completo, nunca um anúncio pela metade. O mutex protege a troca da entrada. O limite de `uint32_t` de `chunk_count` é respeitado: arquivos que exigiriam mais de `UINT32_MAX` chunks recebem erro.

`metadata_find_document` devolve uma cópia independente dos hashes. O chamador usa `file_metadata_free` após a consulta. Isso evita expor ponteiros internos que poderiam perder validade quando o índice mudar.

## 6. Fluxo completo: do upload ao download

> “Primeiro, o Peer faz JOIN e entra na tabela de membros. No upload, o serviço do Peer calcula o ObjectID, divide e armazena o PDF em chunks, calcula os hashes e usa LZ4. Depois de confirmar o armazenamento, ele envia ANNOUNCE. O Super Peer valida a origem, converte o anúncio em `FileMetadata` e registra as localizações. No download, outro Peer faz LOOKUP por nome ou ObjectID. O Super Peer devolve os descritores e consulta a tabela de membros para transformar cada NodeID disponível em IP e porta. O Peer solicitante baixa os chunks diretamente dos Peers indicados, confere os hashes e, ao final, verifica o ObjectID.”

**Ponto central para explicar:** a parte do aluno 2 mantém **identidade, índice e localização**. A transferência dos bytes do PDF é feita diretamente entre Peers, integrada à parte do aluno 1. A estrutura C com ponteiros não é enviada pela rede; o protocolo serializa os dados em mensagens próprias.

```text
Peer A -- JOIN ----------------------> Super Peer: tabela de membros
Peer A -- ANNOUNCE ------------------> Super Peer: hash table e localizações
Peer B -- LOOKUP --------------------> Super Peer: documento, hashes, IP/porta
Peer B <------ chunks do PDF -------- Peer A
Peer B -- verifica hashes/ObjectID --> arquivo concluído
```

## 7. Demonstração no terminal

Compile na raiz do projeto:

```bash
make -B all
```

Abra três terminais para manter o Super Peer e dois Peers ativos:

```bash
# Terminal 1
./bin/superpeer --port 55101 --name superpeer

# Terminal 2
./bin/peer serve 55102 127.0.0.1 55101

# Terminal 3
./bin/peer serve 55103 127.0.0.1 55101
```

Em um quarto terminal, com um PDF chamado `arquivo.pdf` na pasta atual:

```bash
./bin/peer upload arquivo.pdf 127.0.0.1 55102
./bin/peer --local-peer-port 55103 download arquivo.pdf recebido.pdf 127.0.0.1 55101
cmp arquivo.pdf recebido.pdf
```

No upload, mostre `File`, `Size`, `ObjectID`, `Chunks`, os hashes dos chunks e `Compression: LZ4`. No download, mostre `Download completed` e `SHA-256 verified`. O `cmp` confirma que os arquivos são byte a byte iguais. Para os testes automatizados, execute `make test-aluno2`, `make test-c2`, `python3 tests/c2/integration.py --bin-dir bin` e `bash script_testes_c2.sh`. Na verificação de 29/09/2026, a integração teve 46 verificações aprovadas e o script C2 passou 20/20; esses números descrevem aquela execução, não substituem os testes no computador da apresentação.

## 8. Perguntas prováveis

**Por que existem NodeID e ObjectID?** O NodeID identifica um participante da rede a partir de sua configuração. O ObjectID identifica os bytes de um documento. Um mesmo documento pode estar em vários Peers.

**O Super Peer guarda o PDF?** Não. Ele guarda metadados e localizações; os Peers guardam e transferem os chunks.

**O que acontece se dois documentos tiverem o mesmo nome?** A consulta por nome retorna ambiguidade. A consulta por ObjectID identifica o conteúdo exato.

**O que acontece se dois ObjectIDs caírem no mesmo bucket?** Ambos ficam na lista do bucket; a comparação usa todos os bytes do ObjectID.

**Por que usar bibliotecas?** OpenSSL EVP fornece SHA-256 e LZ4 fornece compressão. Reaproveitar essas implementações reduz código próprio e evita reimplementar algoritmos conhecidos.

**O sistema já detecta automaticamente um Peer que caiu?** Não. LEAVE remove uma saída voluntária; heartbeat, Gossip e detecção de falhas não estão implementados.

**O índice sobrevive ao reinício do Super Peer?** Não diretamente: o índice é volátil. Os Peers persistem seus dados e os reanunciam ao reiniciar. Reiniciar só o Super Peer não solicita automaticamente reanúncio dos Peers que continuaram ativos.

**O que ainda falta além do checkpoint 2?** Não há Chord, consenso/SMR, 2PC, IST ou replicação consistente. No caminho de CRC inválido, a recepção fecha a conexão sem enviar ERROR, diferença conhecida em relação ao requisito escrito.

## Fechamento em uma frase

> “O resultado do meu trabalho neste checkpoint é um Super Peer capaz de identificar os participantes, indexar documentos pelo conteúdo e indicar, para cada chunk, quais Peers podem fornecê-lo; a verificação por hashes liga esse índice ao download confiável.”
