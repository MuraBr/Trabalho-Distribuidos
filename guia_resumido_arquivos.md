# Guia resumido dos arquivos do projeto

Este guia explica a responsabilidade de cada arquivo, sem detalhar funções nem reproduzir código. O projeto reúne os componentes dos checkpoints 1 e 2.

## 1. Visão geral

O **Peer** armazena e transfere os documentos. O **Super Peer** mantém o catálogo que informa quais documentos existem e onde estão seus chunks — as partes em que cada arquivo é dividido.

Existem dois executáveis reais: `bin/peer` e `bin/superpeer`. `bin/client` e `bin/node` são aliases, respectivamente, desses programas, para manter compatibilidade com os testes.

## 2. Inicialização e comandos

### `peer.c`

É a entrada do programa Peer. Interpreta comandos como iniciar o serviço, upload e download, encaminhando o trabalho aos módulos responsáveis. Não implementa sozinho toda a transferência.

### `peer_service.c`

Mantém o Peer de armazenamento funcionando: inicia sua identidade, carrega os arquivos salvos, registra-se no Super Peer e atende pedidos de chunks. Integra principalmente `storage.c`, `rpc.c` e `concurrent_server.c`.

### `superpeer.c`

Contém a entrada exclusiva do executável Super Peer. É pequeno porque apenas encaminha a execução para `superpeer_app.c`.

### `superpeer_app.c`

Coordena o funcionamento do Super Peer. Recebe registros de nós, anúncios de documentos e consultas de localização, usando `membership.c` e `directory.c`. Não guarda o conteúdo dos PDFs.

### `app_config.c`

Lê configurações e argumentos, como portas, endereços e diretório de dados. Também mantém o UUID persistente usado na identidade do nó, permitindo recuperar o mesmo NodeID quando os demais componentes da identidade não mudam.

### `local_control.c`

Liga a linha de comando ao Peer que já está ativo, por um socket Unix local. Assim, upload e download são executados pelo serviço com o mesmo NodeID usado no registro, e o progresso volta ao terminal.

## 3. Comunicação

### `network.c`

Cuida dos sockets TCP: abrir servidor, aceitar conexões, conectar, enviar, receber e fechar. Trata transferências parciais e tempos de espera, sem precisar entender documentos ou chunks.

### `protocol.c`

Organiza os bytes TCP em mensagens com header e payload. Faz o framing — a delimitação de cada mensagem — e verifica CRC32 para detectar alterações nos bytes recebidos. Usa `network.c` para transportá-los.

### `transfer_protocol.c`

Define como os dados de upload, consulta e download são representados dentro do payload. Converte documentos, descritores de chunks e localizações entre estruturas C e bytes, além de gerar os identificadores de transação.

### `rpc.c`

Encapsula uma troca de requisição e resposta: conecta, envia, recebe, confere a correlação e fecha a conexão. Também centraliza o formato dos dados enviados no JOIN, evitando repetir essa lógica em vários módulos.

### `concurrent_server.c`

Permite atender várias conexões com um conjunto limitado de threads e uma fila. É compartilhado pelos dois servidores; cada um fornece seu próprio tratamento de mensagem.

## 4. Identidade e catálogo

### `node.c`

Representa a identidade de um nó. Calcula o NodeID a partir de IP, porta e UUID, valida essa composição e converte identificadores para texto. Apesar do nome, não é a entrada de `bin/node`.

### `membership.c`

Mantém a tabela de nós conhecidos pelo Super Peer, incluindo seus endereços. Permite registrar, consultar e remover membros, protegendo a tabela contra acessos simultâneos. Não é o catálogo de documentos.

### `metadata.c`

Mantém o índice de documentos, seus descritores e os NodeIDs que possuem cada chunk. Também calcula o ObjectID, que é o SHA-256 do arquivo inteiro. O índice do Super Peer fica em memória; não contém os bytes dos PDFs.

### `directory.c`

Une o catálogo de documentos à tabela de membros. Em uma consulta, descobre quais nós possuem os chunks e transforma seus NodeIDs em IPs e portas. Usa `metadata.c` e `membership.c` para montar a resposta.

## 5. Arquivos e transferência

### `file_client.c`

Coordena upload e download dentro do Peer executor. No upload, calcula hashes, divide o arquivo, comprime e envia chunks; no download, consulta localizações, busca as partes, verifica e remonta o arquivo. Usa threads para trabalhar com vários chunks.

O nome “client” indica que esse módulo faz solicitações a outros nós; ele não representa um terceiro executável independente.

### `storage.c`

Cuida dos chunks e manifests no disco do Peer. Separa uploads incompletos em `pending` e objetos publicados em `objects`, verifica integridade e recupera o catálogo no reinício. O manifest descreve as partes de um documento armazenado.

### `compression.c`

Encapsula a biblioteca LZ4 para comprimir e descomprimir cada chunk. Não conhece sockets, nomes de documentos ou localização de Peers; recebe bytes e produz bytes.

### `content.c`

Reúne auxiliares para conteúdo e arquivos: SHA-256 de buffers, extração do nome, verificação da extensão e sincronização de diretório. A aceitação de PDF é pela extensão `.pdf`, sem validação estrutural do formato.

## 6. Arquivos `.h`

Em geral, cada `.h` apresenta os tipos e operações disponíveis no `.c` correspondente. Por exemplo, `network.h` é a interface de `network.c`; ele permite que outros módulos usem a rede sem conhecer seus detalhes internos.

Alguns merecem destaque:

- `common.h`: constantes compartilhadas, como tamanhos dos identificadores.
- `transfer_types.h`: estruturas de documento, chunk, endpoint e resultado de consulta, além dos estados da transferência.
- `wire.h`: pequenos auxiliares para representar números em uma ordem de bytes padronizada.
- `remote_error.h`: converte erros locais em códigos estáveis para transmitir pela rede e interpretá-los no receptor.
- `superpeer.h`: interface da tabela de membros implementada em `membership.c`, não apenas da entrada `superpeer.c`.

Os demais headers acompanham os módulos descritos acima: configuração, compressão, conteúdo, servidor concorrente, diretório, transferência, controle local, metadados, rede, identidade, serviço Peer, protocolo, RPC e armazenamento.

## 7. Compilação e testes

| Arquivo | Papel |
| --- | --- |
| `Makefile` | Compila os programas, liga as bibliotecas externas, cria os aliases e oferece alvos de testes. |
| `tests/c1/Makefile` | Compila o teste específico do protocolo C1. |
| `test_node_superpeer.c` | Testa identidade e tabela de membros pela API local, sem comunicação TCP. |
| `tests/c1/test_protocol.c` | Testa transmissão/recepção de mensagens e CRC32. |
| `tests/c2/test_metadata.c` | Testa hashes, documentos e disponibilidade de chunks na API de metadados. |
| `tests/c2/test_transfer_negative.c` | Verifica rejeição de mensagens/chunks inválidos e retomada de upload pendente. |
| `tests/c2/test_storage_atomic.c` | Provoca falha de persistência e verifica se uma gravação malsucedida não é tratada como confirmada. |
| `tests/c2/integration.py` | Inicia processos reais e testa transferências, identidade, concorrência, falhas e reinicialização. |
| `script_testes.sh` | Executa a suíte C1 de comunicação, comandos e logs. |
| `script_testes_peer.sh` | Testa interação legada entre nós usando `bin/node`; não é o fluxo de armazenamento C2. |
| `script_testes_c2.sh` | Testa upload/download, integridade, persistência e situações de erro do C2. |
| `redvidassobretrabalhodepd/env.sh` | Define caminhos e variáveis usados pelos scripts fornecidos/adaptados. |
| `redvidassobretrabalhodepd/common.sh` | Reúne auxiliares de teste, como comparação de arquivos, espera e contagem de resultados. |
| `redvidassobretrabalhodepd/run.sh` | Executa uploads e downloads dos PDFs encontrados e compara os resultados. |
| `redvidassobretrabalhodepd/run.pre-refactor.sh` | Preserva uma versão anterior do runner para consulta; não é a versão recomendada atual. |
| `teste.c` | Demonstração isolada da versão do padrão C; não participa do build principal. |

Esses arquivos descrevem verificações, não garantias de aprovação: o resultado depende da execução. Testes locais de API não substituem testes de integração entre processos.

## 8. Como os arquivos trabalham juntos

### Upload

1. `peer.c` recebe o comando e `local_control.c` o entrega ao Peer ativo.
2. `file_client.c` prepara o documento e envia seus chunks usando compressão, RPC, protocolo e rede.
3. `peer_service.c` recebe os pedidos e `storage.c` verifica e salva as partes.
4. Depois de validar o documento inteiro, o Peer anuncia sua disponibilidade ao Super Peer.
5. `superpeer_app.c`, `directory.c` e `metadata.c` registram onde encontrá-lo.

### Download

1. O comando chega ao Peer ativo pelo mesmo controle local.
2. `file_client.c` consulta o Super Peer, que usa o catálogo e a tabela de membros para devolver localizações.
3. O Peer executor pede os chunks diretamente aos Peers que os armazenam.
4. Descomprime, verifica hashes e remonta o arquivo antes de confirmar a conclusão.

Em resumo: **o Super Peer informa onde buscar; os Peers transferem os bytes**.

## 9. Ordem sugerida de leitura

Comece por `peer.c` e `superpeer.c` para identificar as entradas. Depois leia `peer_service.c` e `superpeer_app.c` para entender os serviços. Siga para `file_client.c`, `storage.c`, `directory.c` e `metadata.c` para acompanhar os documentos. Por último, aprofunde-se em `rpc.c`, `protocol.c` e `network.c` para entender o transporte.

Este resumo não substitui o [guia detalhado de funções](guia_completo_funcoes.md): use aquele apenas quando precisar estudar uma implementação específica. Chord, Gossip, eleição e replicação automática continuam fora do escopo implementado aqui.
