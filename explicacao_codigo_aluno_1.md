# Explicação dos códigos — Aluno 1

Este documento explica a implementação presente no código-fonte em 18/09/2026, com foco no checkpoint 1: comunicação TCP, serialização, framing, CRC32 e atendimento concorrente. Os recursos previstos para checkpoints posteriores são apresentados como pendências, não como funcionalidades concluídas.

## 1. Organização dos arquivos

| Arquivo | Responsabilidade |
|---|---|
| [common.h](common.h) | Constantes compartilhadas de identidade dos nós. |
| [network.h](network.h) e [network.c](network.c) | Interface e implementação das operações de socket TCP. |
| [protocol.h](protocol.h) e [protocol.c](protocol.c) | Estrutura das mensagens, serialização, validação, framing e CRC32. |
| [peer.c](peer.c) | Processo que recebe conexões e também pode iniciar um JOIN em outro peer. |
| [client.c](client.c) | Cliente auxiliar para enviar PING, JOIN e LEAVE. |
| [Makefile](Makefile) | Compilação dos executáveis e execução do teste de protocolo. |
| [tests/c1/test_protocol.c](tests/c1/test_protocol.c) | Teste local de envio e recebimento de uma mensagem. |
| [script_testes.sh](script_testes.sh) | Roteiro automatizado de verificações do checkpoint 1. |
| [teste.c](teste.c) | Programa independente que imprime a versão do padrão C informada pelo compilador. |

Os arquivos `node.c` e `superpeer.c` pertencem ao aluno 2. O aluno 1 chama suas APIs para criar a identidade local e cadastrar membros, mas implementa a comunicação que transporta essas informações entre processos.

## 2. Como as camadas se relacionam

```text
peer.c / client.c
    │ escolhem a mensagem e preenchem seus campos
    ▼
protocol.c
    │ transforma campos em bytes e verifica a integridade
    ▼
network.c
    │ envia e recebe os bytes pelo socket
    ▼
TCP/IP
```

Na recepção, o caminho é inverso: a rede entrega bytes, o protocolo reconstrói uma mensagem válida e o peer decide o que fazer com ela.

TCP é um fluxo de bytes. Um único `send()` não corresponde necessariamente a um único `recv()`: os dados podem chegar em partes ou agrupados. Por isso, o projeto combina repetição das operações de rede com um header que informa o tamanho exato do payload.

## 3. `common.h`: constantes compartilhadas

- `NODE_ID_SIZE`: 32 bytes, tamanho binário do resultado SHA-256 usado como NodeID.
- `NODE_UUID_SIZE`: 16 bytes, tamanho do UUID usado na formação da identidade.
- `NODE_ADDRESS_SIZE`: `INET6_ADDRSTRLEN`, que fornece 46 bytes para um endereço IP textual e seu terminador.

A reserva de espaço para texto IPv6 não significa que o transporte atual suporte IPv6. `network.c` utiliza sockets `AF_INET`, portanto trabalha com IPv4.

## 4. `network.c`: comunicação TCP

`network.h` declara as funções públicas. O restante do projeto pode utilizá-las sem repetir a configuração de sockets.

### 4.1. `network_create_server`

Cria o socket de escuta em cinco etapas:

1. `socket(AF_INET, SOCK_STREAM, 0)` cria um socket TCP IPv4.
2. `setsockopt()` configura `SO_REUSEADDR`, permitindo reutilização do endereço nas condições aceitas pelo sistema operacional.
3. Preenche `sockaddr_in` com `INADDR_ANY` e a porta convertida por `htons()`.
4. `bind()` associa o socket à porta local.
5. `listen()` coloca o socket em modo de escuta.

`INADDR_ANY` permite receber conexões pelas interfaces IPv4 da máquina. O argumento `backlog` controla a fila de conexões pendentes; no peer, o valor solicitado é 10. Ele não representa um limite de dez clientes já atendidos.

A função retorna o descritor do servidor ou `-1`. Se ocorrer uma falha depois da criação, fecha o socket antes de retornar.

### 4.2. `network_accept_client`

Chama `accept()` e retorna um novo descritor para a conexão aceita. O descritor original continua reservado para aceitar outras conexões.

A chamada é bloqueante. Erros retornam `-1`; o chamador decide como lidar com interrupções como `EINTR`.

### 4.3. `network_connect`

Cria um socket, converte o IPv4 textual com `inet_pton()` e chama `connect()` para alcançar o servidor. Retorna o descritor conectado ou `-1`, fechando o socket nos caminhos de erro.

O endereço precisa ser um IPv4 numérico, como `127.0.0.1`. Não há resolução de nomes como `localhost` nessa função.

### 4.4. `network_send_all`

Repete `send()` até enviar todos os bytes do buffer. A cada envio parcial, avança o ponteiro e reduz a quantidade restante.

Se ocorrer `EINTR`, tenta novamente. Outros erros, ou um envio que retorna zero, produzem `-1`. No sucesso, retorna o total enviado.

### 4.5. `network_recv_exact`

Repete `recv()` até receber a quantidade pedida ou encontrar fechamento/erro:

| Retorno | Significado |
|---|---|
| Quantidade solicitada | Buffer recebido por completo. |
| Quantidade positiva menor que a solicitada | Conexão fechada durante a leitura. |
| Zero | Conexão fechada antes de receber qualquer byte dessa leitura. |
| `-1` | Erro de recepção. |

Assim como no envio, `EINTR` causa uma nova tentativa. Não há timeout configurado nessas funções; uma conexão aberta sem enviar os bytes restantes pode manter a leitura bloqueada.

### 4.6. `network_shutdown`

Tenta encerrar os dois sentidos da comunicação com `shutdown(sock, SHUT_RDWR)` e chama `close()` mesmo se o primeiro passo falhar. O retorno informa o resultado do fechamento com `close()`.

## 5. `protocol.h`: representação das mensagens

Uma `Message` contém um `Header` e um ponteiro para o payload. O header descreve a mensagem; o payload carrega seus dados específicos.

As constantes principais são:

- `PROTOCOL_VERSION = 1`: versão aceita.
- `TRANSACTION_ID_SIZE = 16`: tamanho do identificador de transação.
- `HEADER_WIRE_SIZE = 98`: quantidade de bytes do header transmitido.
- `MAX_PAYLOAD_SIZE = 4 * 1024 * 1024`: limite de 4 MiB por payload.
- `JOIN_PAYLOAD_WIRE_SIZE = 64`: tamanho do descritor usado no JOIN e no ACK desse JOIN.

O limite de payload não implementa fragmentação de arquivos. Ele apenas limita o corpo de cada mensagem e a alocação correspondente.

### 5.1. Layout do header na rede

Os deslocamentos abaixo começam em zero:

| Campo | Deslocamento | Bytes | Uso |
|---|---:|---:|---|
| `protocol_version` | 0 | 1 | Versão do protocolo. |
| `message_type` | 1 | 1 | Operação solicitada ou resposta. |
| `source_node` | 2 | 32 | NodeID de origem. |
| `destination_node` | 34 | 32 | NodeID de destino. |
| `transaction_id` | 66 | 16 | Correlação entre pedido e resposta. |
| `timestamp` | 82 | 8 | Horário preenchido pelo emissor. |
| `payload_size` | 90 | 4 | Tamanho do corpo em bytes. |
| `checksum` | 94 | 4 | CRC32 do header e payload. |

Os inteiros de múltiplos bytes são transmitidos em big-endian, com o byte mais significativo primeiro. Os identificadores são copiados como sequências de bytes.

A estrutura C não é enviada diretamente: o compilador pode inserir preenchimento entre os campos. A serialização manual garante um formato de 98 bytes, independentemente desse preenchimento.

### 5.2. Tipos definidos

| Valor | Tipo | Tratamento atual pelo servidor |
|---:|---|---|
| 0 | `M_JOIN` | Valida e cadastra o nó; responde ACK ou ERROR. |
| 1 | `M_ACK` | É uma resposta; se chegar como pedido ao servidor, recebe ERROR. |
| 2 | `M_ERROR` | É uma resposta; se chegar como pedido ao servidor, recebe ERROR. |
| 3 | `M_PING` | Recebe PONG. |
| 4 | `M_PONG` | É uma resposta; se chegar como pedido ao servidor, recebe ERROR. |
| 5 | `M_LEAVE` | Recebe ACK, sem remover membro. |

Um tipo aceito pelo protocolo não precisa ter um tratamento de aplicação próprio. Tipos fora dessa tabela são rejeitados antes do processamento pelo peer.

## 6. `protocol.c`: serialização, framing e CRC32

### 6.1. Inicialização e memória

`message_init()` zera a mensagem e define a versão do protocolo. Ela não libera um payload anterior. `message_free()` libera o payload e zera os campos.

O chamador deve liberar uma mensagem recebida antes de reutilizá-la. Um payload entregue a `message_free()` deve ser uma alocação compatível com `free()`, não um vetor local da pilha.

### 6.2. Conversão dos campos

`write_u32_be()` e `write_u64_be()` escrevem inteiros em big-endian. `read_u32_be()` e `read_u64_be()` fazem a conversão inversa.

`protocol_serialize_header()` valida o header e copia cada campo para sua posição no buffer. `protocol_deserialize_header()` reconstrói os campos, mas não faz a validação semântica por conta própria. Ambas retornam 98 no sucesso ou um valor negativo em erro.

`protocol_validate_header()` verifica versão, tipo permitido e limite de payload. Ela não verifica CRC32 nem a identidade dos nós; essas verificações acontecem em outras etapas.

### 6.3. Cálculo de CRC32

O projeto usa `crc32()` da biblioteca zlib. A função interna `crc32_update()` processa os dados em blocos que cabem no tipo de tamanho aceito pela biblioteca.

`protocol_calculate_crc32()` calcula o CRC de um buffer. Já `protocol_message_crc32()` calcula o CRC da mensagem inteira:

1. Copia o header e zera o campo `checksum` nessa cópia.
2. Serializa a cópia.
3. Calcula o CRC dos 98 bytes serializados.
4. Continua o cálculo sobre o payload, quando existe.

Zerar o checksum evita que o resultado dependa do próprio valor que está sendo calculado. O receptor repete o procedimento e compara os valores. CRC32 detecta corrupção de dados; não autentica o emissor.

### 6.4. Envio de mensagem

`protocol_send_message()` valida o header e exige payload não nulo quando o tamanho é positivo. Calcula o checksum em uma cópia do header, envia os 98 bytes e depois envia o corpo usando `network_send_all()`.

O checksum da estrutura original do chamador não é atualizado: a alteração ocorre somente na cópia usada no envio.

### 6.5. Recepção e framing

`protocol_receive_message()` executa esta sequência:

1. Lê exatamente 98 bytes de header.
2. Desserializa e valida os campos antes de alocar o corpo.
3. Aloca e lê exatamente `payload_size` bytes, se necessário.
4. Recalcula o CRC32 e compara com o recebido.
5. Entrega o header e o payload ao chamador apenas no sucesso.

Isso permite ler mensagens consecutivas na mesma conexão, mesmo quando o TCP entrega seus bytes em agrupamentos diferentes.

Os resultados são `PROTOCOL_OK` (0), `PROTOCOL_ERROR` (-1) e `PROTOCOL_CLOSED` (1). O último indica fechamento antes de começar um novo header. Fechamento no meio do header ou do corpo é erro, pois a mensagem ficou incompleta.

## 7. `peer.c`: servidor, cliente e integração

### 7.1. Estruturas de controle

`PeerContext` reúne socket de escuta, identidade local, ponteiro para o Super Peer, mutex, variável de condição e lista de conexões ativas.

Cada `ClientContext` representa uma conexão, guardando seu socket, uma referência ao peer e o próximo elemento da lista. `NodeArguments` guarda as opções de inicialização.

### 7.2. Argumentos e identidade

`parse_port()` aceita portas de 1 a 65535. `parse_node_arguments()` aceita a forma posicional ou opções nomeadas:

```bash
./bin/node 5000
./bin/node 5001 127.0.0.1 5000
./bin/node --config tests/config/c1.conf --port 5000 --name servidor
```

Atualmente, `--config` apenas verifica se o arquivo pode ser acessado para leitura; seu conteúdo não é interpretado. O nome aparece na identificação exibida, enquanto a identidade local usa `127.0.0.1`, a porta informada e o UUID gerado pela API do aluno 2.

`initialize_local_identity()` cria o `Node` e o `SuperPeer` com o mesmo UUID, mantendo o mesmo NodeID local nos dois objetos. `print_node_id()` mostra os bytes em hexadecimal; na rede, continuam sendo 32 bytes binários.

### 7.3. Payload de JOIN e ACK

O descritor possui este formato:

| Deslocamento | Bytes | Conteúdo |
|---:|---:|---|
| 0 | 46 | IP textual, terminador NUL e preenchimento com zeros. |
| 46 | 2 | Porta em ordem de rede. |
| 48 | 16 | UUID. |

`encode_join_payload()` escreve o descritor. `encode_join_payload_alloc()` aloca o buffer e usa essa função. `decode_join_payload()` exige 64 bytes, verifica o terminador e o preenchimento, converte a porta e chama a API de configuração do aluno 2.

Nenhuma `struct Node` é transmitida diretamente. O receptor reconstrói o nó usando os campos serializados.

### 7.4. Registro de um JOIN recebido

`register_join()` aceita destino zerado, usado na descoberta inicial, ou igual ao NodeID local. Depois reconstrói a identidade remota a partir do descritor e compara o NodeID calculado com a origem informada no header.

Também rejeita o próprio nó como membro remoto. Se essas verificações passarem, chama `superpeer_register_node()`. O resultado pode representar inserção ou atualização de um membro existente.

`send_reply()` preserva o TransactionID, usa a identidade local como origem e a origem do pedido como destino. O ACK de JOIN inclui o descritor local; ERROR, PONG e ACK de LEAVE são enviados sem corpo.

### 7.5. JOIN iniciado pelo peer

`connect_and_join()` abre uma conexão, envia JOIN com destino zerado e aguarda a resposta. Para aceitar o ACK, verifica:

- tipo da resposta;
- TransactionID igual ao enviado;
- destino igual à identidade local;
- descritor remoto válido e consistente com o NodeID de origem;
- identidade remota diferente da local;
- sucesso do registro remoto na tabela local.

Ao final, libera as mensagens e fecha essa conexão. O peer continua com seu servidor ativo. Se o JOIN inicial falhar, o `main()` informa a falha, mas mantém o processo atendendo conexões.

```text
Peer B                                  Peer A
  │                                       │
  ├── JOIN + descritor de B ──────────────►│
  │                               valida e registra B
  │◄──────────── ACK + descritor de A ─────┤
  │                                       │
valida e registra A                       │
  └── fecha a conexão usada no JOIN       │
```

Esse é o vínculo entre rede e API local: as mensagens atravessam TCP, mas o cadastro efetivo ocorre por uma chamada local a `superpeer_register_node()` em cada processo.

### 7.6. TransactionID

`fill_transaction_id()` forma 16 bytes: 8 de timestamp, 4 de PID e 4 de sequência local. Os componentes são escritos em ordem de rede.

O receptor trata o resultado como uma sequência opaca e a devolve na resposta. Essa composição implementa correlação, mas não é uma garantia de unicidade global entre máquinas e não inclui o NodeID mencionado na composição conceitual dos requisitos.

### 7.7. Threads e ciclo de atendimento

`accept_clients()` aceita conexões, cria um `ClientContext` e inicia uma thread `handle_client()` por conexão. A thread é destacada com `pthread_detach()`.

`handle_client()` lê mensagens repetidamente no mesmo socket e aplica os tratamentos de JOIN, PING e LEAVE. A conexão termina quando o cliente fecha, quando o protocolo detecta erro ou quando ocorre falha no envio da resposta.

Uma falha semântica de JOIN, como identidade incompatível com o descritor, gera `M_ERROR`. Header inválido, CRC incorreto ou mensagem incompleta fazem a recepção falhar e a conexão ser encerrada, sem resposta `M_ERROR`.

`track_client()` e `untrack_client()` mantêm a lista de conexões sob mutex. A variável de condição permite esperar a lista esvaziar. Esse mutex protege o acompanhamento das conexões; o cadastro de membros utiliza a API do Super Peer.

### 7.8. Encerramento

O tratador de `SIGINT` e `SIGTERM` altera `g_running`. `SIGPIPE` é ignorado para evitar que uma escrita em conexão encerrada termine imediatamente o processo.

No caminho de encerramento do `main()`, o programa fecha o socket de escuta, espera a thread de aceitação, fecha os sockets acompanhados, aguarda as threads de clientes e destrói os recursos compartilhados. `close_tracked_clients()` e `wait_for_clients()` participam desse controle.

Essa descrição corresponde ao fluxo do código; não substitui testes de encerramento sob concorrência ou com leituras bloqueadas.

## 8. `client.c`: cliente auxiliar

`parse_arguments()` exige comando, host e porta. `parse_command()` reconhece `ping`, `join` e `leave`; `parse_port()` valida a porta.

O cliente conecta, prepara uma mensagem, preenche TransactionID e timestamp, envia e espera uma resposta. Confere a recepção pelo protocolo, o tipo esperado e o TransactionID. Imprime `RX PONG` ou `RX ACK` no sucesso e libera os recursos.

O JOIN usa uma identidade de teste com IP `127.0.0.1` e porta 1. O cliente não abre um servidor nessa porta. Portanto, esse comando testa o cadastro e a resposta, não a disponibilidade de um novo peer para receber conexões.

Diferentemente de `connect_and_join()`, o cliente auxiliar não reconstrói e valida a identidade do servidor a partir do ACK. PING e LEAVE também não preenchem uma identidade de origem específica, permanecendo com os bytes zerados da inicialização.

O comando PING envia o texto `PING` no payload, com exatamente 4 bytes, sem terminador NUL. No servidor, o log desse tipo omite a origem e mostra `Mensagem recebida: tipo=3, payload=4 bytes`; os demais tipos continuam exibindo a origem. O tamanho registrado corresponde somente ao payload, além dos 98 bytes de cabeçalho do protocolo.

## 9. Compilação e execução

O Makefile principal gera `bin/node`, a partir de `peer.c` e dos módulos de apoio, e `bin/client`, a partir de `client.c` e suas dependências. O alvo padrão utiliza C2x e opções de avisos do compilador.

As bibliotecas utilizadas são POSIX Threads (`-pthread`), zlib (`-lz`) e libcrypto, vinculada por padrão com `-Wl,-l:libcrypto.so.3`. O SHA-256 da identidade é chamado em `node.c`; o CRC32 das mensagens fica em `protocol.c`.

Compile na raiz do projeto:

```bash
make
```

Para observar o JOIN entre dois peers, execute em terminais separados:

```bash
./bin/node 5000
```

```bash
./bin/node 5001 127.0.0.1 5000
```

Para usar o cliente auxiliar com o primeiro servidor:

```bash
./bin/client --cmd ping --host 127.0.0.1 --port 5000
./bin/client --cmd join --host 127.0.0.1 --port 5000
./bin/client --cmd leave --host 127.0.0.1 --port 5000
```

Use `Ctrl+C` para solicitar o encerramento dos servidores. Os exemplos usam loopback; a identidade anunciada está fixa nesse endereço no código atual.

## 10. Testes existentes e alcance das verificações

### 10.1. Teste local do protocolo

`tests/c1/test_protocol.c` cria um `socketpair(AF_UNIX, SOCK_STREAM, 0, ...)`, envia uma mensagem PING com payload e verifica tipo, timestamp, tamanho, TransactionID e conteúdo recebido.

O teste exercita serialização, envio, recepção e a validação de CRC no caminho de sucesso. Não abre conexão TCP/IP nem inicia dois processos independentes. Também não injeta corrupção, força fragmentação ou verifica múltiplas mensagens consecutivas.

```bash
make test
```

O alvo compila a aplicação e executa esse teste local; não executa automaticamente o script de integração.

### 10.2. Script do checkpoint 1

```bash
bash script_testes.sh
```

O script recompila o teste local, inicia `bin/node`, utiliza `bin/client` e verifica respostas PONG/ACK, vinte clientes concorrentes, registros de inicialização e uma tentativa de envio de versão inválida. A porta padrão é 55101, ajustável por `C1_PORT`.

Há dois limites relevantes na interpretação desse roteiro:

- A verificação chamada de framing conta PINGs no log; não comprova sozinha fragmentação e agrupamento de mensagens na mesma conexão.
- O trecho Python de versão inválida usa `!BH`, embora versão e tipo reais tenham um byte cada. Além disso, trata exceções de recepção como ausência de dados. Seu resultado isolado não comprova uma rejeição correta e imediata do formato esperado.

### 10.3. Evidências desta documentação

Este documento foi elaborado pela leitura dos fontes, cabeçalhos, Makefiles e testes. Não foram executadas compilação nem suítes de teste para sua elaboração. Os comandos acima são instruções de reprodução, não resultados obtidos nesta tarefa.

Permanecem sem nova verificação nesta tarefa: integração TCP entre peers, rejeição de CRC inválido, mensagens truncadas, enquadramento de mensagens consecutivas, concorrência e encerramento com clientes ativos.

## 11. Limites atuais e próximos checkpoints

O código analisado implementa a base de comunicação e integração de cadastro do checkpoint 1. É necessário distinguir essa base dos recursos seguintes:

| Tema | Estado observado |
|---|---|
| JOIN e cadastro remoto | Fluxo de rede chama a API local de cadastro e responde ACK/ERROR. |
| PING/PONG | Troca pontual de mensagens; não há agendamento de heartbeat nesse fluxo. |
| LEAVE | Confirmação com ACK; não remove membro da tabela. |
| Configuração por arquivo | Opção reconhecida; conteúdo não interpretado pelo peer. |
| Upload, download e chunks | Não implementados nos módulos do aluno 1 analisados. |
| SHA-256 de documentos e LZ4 | Não implementados nesse fluxo; SHA-256 atual é usado para identidade pelo aluno 2. |
| Estados de falha e recuperação | A comunicação descrita não implementa o ciclo completo de detecção de falhas. |
| ELECTION, OK e COORDINATOR | Não definidos entre os tipos atuais de `protocol.h`. |
| Cache LFU e transferência paralela | Pendentes nos módulos analisados. |

Para apresentar o código, a sequência sugerida é: explicar o fluxo TCP, mostrar o header de 98 bytes, demonstrar como o tamanho delimita o payload, explicar o CRC32 e percorrer o JOIN até o cadastro pela API do aluno 2.
