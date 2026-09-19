
## `peer.c` — execução do servidor e integração

Coordena o nó, aceita conexões TCP e trata mensagens de clientes em threads. Integra os módulos de rede, protocolo, identidade e tabela de membros.

- `main()`: prepara a identidade local, inicia o servidor e coordena o encerramento.
- `accept_clients()`: aceita conexões e inicia uma thread para cada cliente.
- `handle_client()`: recebe mensagens e trata JOIN, PING e LEAVE. O log de PING omite a origem.
- `register_join()`: valida a identidade recebida no JOIN e registra o membro na tabela local.
- `connect_and_join()`: conecta a outro nó, envia JOIN e valida o ACK e a identidade do servidor.
- `send_reply()`: monta e envia respostas, como ACK, PONG e ERROR.

## `client.c` — cliente auxiliar de comandos

Permite enviar `ping`, `join` e `leave` a um servidor para exercitar o protocolo.

- `main()`: conecta, monta o comando, envia a mensagem e confere o tipo e o TransactionID da resposta.
- `parse_arguments()`: interpreta comando, endereço e porta informados no terminal.
- `encode_join_payload()`: organiza IP, porta e UUID no corpo do JOIN.
- `fill_transaction_id()`: preenche o identificador que relaciona solicitação e resposta.

O PING envia o texto `PING`, com 4 bytes e sem terminador NUL. O JOIN utiliza uma identidade de teste; o cliente não abre um servidor no endereço anunciado.

## `network.c` — comunicação TCP

Encapsula operações de sockets IPv4 e a transferência de bytes.

- `network_create_server()`: cria o socket, associa a porta e inicia a escuta.
- `network_accept_client()`: aceita uma conexão de entrada.
- `network_connect()`: conecta a um endereço IPv4 e uma porta.
- `network_send_all()`: repete o envio para lidar com escritas parciais.
- `network_recv_exact()`: lê até completar a quantidade solicitada, ocorrer erro ou a conexão fechar.
- `network_shutdown()`: encerra a comunicação e fecha o socket.

## `protocol.c` — formato e integridade das mensagens

Converte mensagens em bytes e reconstrói mensagens recebidas. Usa um cabeçalho de 98 bytes, tamanho explícito do payload e checksum CRC32.

- `message_init()` e `message_free()`: inicializam mensagens e liberam o payload alocado.
- `protocol_serialize_header()` e `protocol_deserialize_header()`: convertem os campos do cabeçalho para o formato de rede e de volta.
- `protocol_validate_header()`: verifica versão, tipo de mensagem e limite do payload.
- `protocol_send_message()`: calcula o CRC32 e envia cabeçalho e payload.
- `protocol_receive_message()`: recebe a mensagem completa e valida cabeçalho e CRC32.
- `protocol_calculate_crc32()`: calcula o CRC32 de um buffer.

## `node.c` — configuração e identidade dos nós

Fornece uma API local para representar nós. Valida endereços IPv4/IPv6 e calcula o NodeID com SHA-256 sobre IP binário, porta em ordem de rede e UUID. Não abre conexões.

- `node_generate_uuid()`: gera o UUID usado na identidade.
- `node_config_init()` e `node_config_init_with_uuid()`: preparam a configuração com UUID novo ou fornecido.
- `node_compute_id()`: calcula o NodeID de 32 bytes.
- `node_init()`: inicializa o nó, seu identificador, PID e papel inicial de peer.
- `node_validate()`: verifica a consistência da configuração, identidade, PID e papel.
- `node_id_to_hex()` e `node_id_from_hex()`: convertem o identificador entre bytes e texto hexadecimal.
- `node_id_compare()` e `node_id_equal()`: comparam identificadores.

## `superpeer.c` — tabela local de membros

Gerencia uma tabela em memória protegida por mutex. É uma API local; a integração com mensagens da rede acontece em `peer.c`.

- `superpeer_create()`: cria o Super Peer e inclui o próprio nó na tabela.
- `superpeer_register_node()`: adiciona um membro ou atualiza um registro existente sem duplicá-lo.
- `superpeer_find_member()`: consulta um membro pelo NodeID.
- `superpeer_unregister_node()`: remove um membro, impedindo a remoção do próprio Super Peer.
- `superpeer_member_count()` e `superpeer_is_registered()`: consultam quantidade e presença de membros.
- `superpeer_destroy()`: libera a tabela e os recursos de sincronização.

A remoção está disponível nessa API, mas ainda não é acionada pelo LEAVE recebido via TCP. O módulo também não implementa heartbeat periódico ou eleição distribuída.

## `test_node_superpeer.c` — testes das APIs locais

Verifica identidade, configuração e operações da tabela de membros por chamadas diretas, sem comunicação TCP.

- `test_node_id_is_deterministic()`: compara o NodeID com um hash esperado para entradas fixas.
- `test_node_records_process_and_round_trips_id()`: verifica PID, papel, IPv6 e conversão hexadecimal.
- `test_node_rejects_invalid_configuration()`: verifica a rejeição de configurações inválidas.
- `test_superpeer_registers_and_finds_members()`: verifica cadastro, consulta, expansão e atualização sem duplicação.
- `test_superpeer_rejects_inconsistent_nodes_and_unregisters()`: verifica identidade inconsistente, remoção e proteção do nó local.
- `test_superpeer_members_are_thread_safe()`: exercita cadastros concorrentes com quatro threads; não prova ausência de todas as possíveis condições de corrida.

## `tests/c1/test_protocol.c` — teste de ida e volta do protocolo

Exercita envio e recepção usando `socketpair(AF_UNIX, SOCK_STREAM, ...)`, sem estabelecer uma conexão TCP real.

- `test_message_round_trip()`: envia uma mensagem com payload e compara tipo, timestamp, tamanho, TransactionID e conteúdo recebido.
- `main()`: executa o teste e informa sucesso se nenhuma asserção falhar.

## `teste.c` — consulta do padrão C

Programa auxiliar independente da aplicação distribuída.

- `main()`: imprime `__STDC_VERSION__`, valor que indica a versão do padrão C informada pelo compilador.

## Relação entre os módulos e verificações

No envio, `peer.c` ou `client.c` monta uma mensagem, `protocol.c` a serializa e `network.c` transmite os bytes. Na recepção, o caminho se inverte. `node.c` fornece identidades e `superpeer.c` mantém o cadastro local.

Este guia foi conferido por leitura do código; sua criação não executou novos testes. Na alteração anterior do PING, passaram `make test` e uma verificação TCP local do payload `PING`, da resposta PONG e do log sem origem. A suíte `test_node_superpeer.c` não foi reexecutada nessa verificação.
