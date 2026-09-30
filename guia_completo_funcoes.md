# Guia completo das funções do projeto

Este guia registra uma revisão anterior do código em 28–29/09/2026. O catálogo detalhado inclui cópias históricas de corpos de função e assinaturas. Após a mudança para `FileMetadata` e a consolidação dos executáveis em 29/09, suas seções de `metadata.h`, `metadata.c`, `directory.c`, `tests/c2/test_metadata.c`, `membership.c`, `superpeer_app.c`, `peer_service.c`, `peer.c` e `superpeer.c` não correspondem mais aos fontes. Para a organização vigente, leia [guia_resumido_arquivos.md](guia_resumido_arquivos.md), [explicacao_codigo_aluno_2.md](explicacao_codigo_aluno_2.md), [requisitos_aluno_2.md](requisitos_aluno_2.md) e os arquivos C atuais. O restante documenta as responsabilidades e chamadas da revisão anterior.

## Navegação rápida

- [Mapa do sistema e interfaces](#1-antes-das-funções-como-o-programa-está-dividido)
- [Mecanismos explicados](#25-entendendo-os-mecanismos-antes-de-abrir-o-catálogo)
- [Catálogo de todas as funções C](#26-catálogo-detalhado--todas-as-234-funções-c)
- [Python e Bash](#27-python-e-bash--funções-e-execução-dos-scripts)
- [Headers e build](#28-headers-tipos-e-regras-de-compilação)
- [Diagnóstico dos scripts](#29-diagnóstico-prático-dos-erros-relatados-durante-esta-revisão)
- [Exercícios de estudo](#30-exercícios-para-estudar-sem-decorar-o-código)

## 1. Antes das funções: como o programa está dividido

O Makefile produz dois programas reais: **bin/superpeer**, cujo main exclusivo está em **superpeer.c**, e **bin/peer**, cujo main está em **peer.c**. **bin/node** aponta para bin/superpeer e **bin/client** aponta para bin/peer. O antigo **client.c** foi removido; o pequeno **teste.c** permanece fora do build padrão. O atendimento TCP do Super Peer fica em **superpeer_app.c**, chamado pelo main de superpeer.c.

Fluxo resumido:

1. O Super Peer inicia sua identidade, tabela de membros e índice de metadados.
2. O Peer de armazenamento carrega seu UUID e manifests, prepara o listener, faz JOIN no Super Peer e reanuncia objetos finalizados.
3. No upload, a CLI entrega o comando ao Peer ativo pelo canal Unix; o serviço calcula o ObjectID, divide o PDF em chunks de 4 MiB, calcula hashes, comprime com LZ4 e envia STORE ao Peer.
4. O Peer verifica cada chunk, persiste em pending, valida o documento inteiro no COMMIT, publica em objects e anuncia ao Super Peer.
5. No download, o Peer ativo faz LOOKUP no Super Peer, recebe localizações e busca cada chunk diretamente dos Peers. Descomprime, verifica hashes e publica o destino somente após conferir o ObjectID completo.

Vocabulário importante: **NodeID** identifica uma instância de nó e deriva de IP binário, porta e UUID; **ObjectID** é o SHA-256 do arquivo original inteiro; **TransactionID** correlaciona uma requisição com sua resposta; **chunk** é uma fatia do arquivo original; **manifest** é o índice persistente dos chunks de um Peer.

Convenção geral: salvo exceções indicadas, funções que retornam int usam 0 para sucesso e -1 para erro, muitas vezes preenchendo errno. Um ponteiro alocado devolvido por uma função passa a pertencer ao chamador. As funções static são privadas ao próprio arquivo C; não são exportadas pelo header.

### Como ler os tipos e retornos usados no guia

- Um **descritor int** de socket/arquivo é um identificador do sistema operacional, não o conteúdo do arquivo. Quem abre ou aceita um descritor precisa fechá-lo; network_shutdown centraliza isso para sockets.
- **uint8_t** representa um byte de dados; uint16_t/uint32_t/uint64_t têm largura exata. **size_t** mede comprimentos na memória; **ssize_t** permite quantidade lida/escrita ou -1.
- Um parâmetro com **const** não deve ser modificado pela função. Um parâmetro **output** recebe resultado por ponteiro; conferir o retorno antes de usar esse resultado evita ler dados parciais.
- **errno** descreve a causa de muitos retornos -1. Não examine errno após sucesso: ele pode conservar um valor antigo.
- **pthread_mutex_t** evita que threads alterem o mesmo catálogo ao mesmo tempo. Não faz rede mais rápida; garante coerência da estrutura compartilhada.
- **static** em uma função C significa visibilidade restrita ao arquivo. Não confunda com uma variável static, que conserva valor entre chamadas.
- **big-endian** significa escrever o byte mais significativo primeiro. O protocolo de rede e o manifest usam um formato definido, mesmo em máquinas que armazenam inteiros internamente em outra ordem.

## 2. Arquivos de interface (.h)

Headers declaram tipos, constantes e protótipos. As exceções são `wire.h` e `remote_error.h`, com pequenos auxiliares `static inline`. A implementação de cada protótipo está explicada na seção do .c correspondente.

| Header | Contrato principal |
| --- | --- |
| **common.h** | Tamanhos compartilhados: NodeID de 32 bytes, UUID de 16 e buffer textual para IP. |
| **network.h** | API TCP: criar servidor, aceitar, conectar, enviar tudo, receber exatamente e fechar. |
| **protocol.h** | Header de 98 bytes, Message, códigos M_JOIN…M_STATE_TRANSFER, limite de payload de 5 MiB e framing/CRC32. Tipos futuros são reconhecidos, mas isso não significa que seus fluxos estejam implementados. |
| **transfer_protocol.h** | Operações de STORE/LOOKUP/DOWNLOAD, estados e descritores de documento, chunk e endpoint; declara encoders/decoders dos payloads. |
| **compression.h** | Encapsulamento de compressão e descompressão LZ4. |
| **content.h** | Validação simples de PDF, SHA-256 de bytes e extração de basename. |
| **node.h** | NodeConfig, NodeID, NodeRole, Node e API de identidade. |
| **superpeer.h** | SuperPeer opaco, membros e API da tabela de membership. |
| **metadata.h** | ObjectID, MetadataDocument, chunk de 4 MiB e índice de documentos/disponibilidade. |
| **directory.h** | Ponte entre metadata e membership para ANNOUNCE/LOOKUP. |
| **storage.h** | Catálogo persistente local do Peer e suas operações BEGIN/CHUNK/COMMIT/leitura. |
| **concurrent_server.h** | Runtime com pool fixo de 32 workers e fila limitada de 64 conexões; callback de atendimento. |
| **rpc.h** | Uma chamada TCP requisição–resposta e o payload de JOIN. |
| **peer_service.h** | Inicialização do Peer em modo servidor de armazenamento. |
| **file_client.h** | Upload, download e benchmark iniciados pelo usuário. |

O campo Header.source_node é um NodeID **declarado na mensagem**, não o IP observado pelo socket. O log ip_origem de superpeer_app.c vem de getpeername. Nas operações C2 normais, a CLI encaminha o comando ao Peer ativo e o source_node é o NodeID desse serviço, o mesmo usado no JOIN. Requisições C2 com origem nula são rejeitadas. Alguns comandos legados de C1 ainda usam IDs nulos; isso não descreve o fluxo de upload/download.

## 3. Rede básica — network.c

- **network_create_server**: Cria TCP/IPv4, SO_REUSEADDR, bind no endereço configurado e listen; retorna socket de escuta ou fecha-o em erro. Backlog é a fila do kernel, não o número de workers.
- **network_accept_client**: Aceita e devolve uma conexão; não altera o socket de escuta. O chamador fecha o descritor recebido.
- **network_connect**: Valida IPv4 numérico e porta; conecta temporariamente em modo não bloqueante e espera POLLOUT com prazo monotônico, verifica SO_ERROR e restaura flags. Fecha o descritor preservando errno em falha.
- **network_send_all(sock, buffer, tam)**: repete send até transmitir todos os bytes, pois uma chamada isolada pode enviar apenas parte deles. Trata EINTR e usa MSG_NOSIGNAL para não derrubar o processo em conexão quebrada. Devolve a quantidade total ou -1.
- **network_recv_exact(sock, buffer, tam)**: repete recv até obter exatamente tam bytes. Se o remoto fecha antes, devolve a quantidade parcial, inclusive 0 quando não recebeu nada; em erro devolve -1. O protocolo considera um header/payload parcial inválido.
- **network_shutdown**: Executa shutdown e close; não imprime erro para sockets já desconectados. O retorno reflete close.

## 4. Header, framing e CRC — protocol.c

O TCP entrega fluxo de bytes, não mensagens. O protocolo primeiro transmite 98 bytes de header. O campo payload_size informa quantos bytes ler em seguida. Os inteiros são serializados em big-endian e o checksum é CRC32 do header com checksum zerado, seguido do payload.



- **crc32_update(crc, data, size)**: chama crc32 da zlib em blocos que cabem no tipo uInt da biblioteca. Permite continuar o cálculo em dados longos.
- **protocol_message_crc32(header, payload)**: copia o header, zera seu checksum, serializa os campos canônicos e calcula CRC32 sobre header mais corpo. Assim remetente e receptor calculam exatamente a mesma sequência de bytes.
- **message_init(message)**: zera a struct e define PROTOCOL_VERSION. Não libera um payload antigo; use message_free antes de reinicializar uma mensagem já preenchida.
- **message_free(message)**: libera message.payload e limpa o header. É a forma normal de liberar um Message recebido ou uma resposta construída com payload próprio.
- **protocol_serialize_header(header, buffer, capacidade)**: valida o header e escreve campo a campo seus 98 bytes no buffer. Retorna o número de bytes escritos ou PROTOCOL_ERROR; nunca envia sizeof(Header) diretamente.
- **protocol_deserialize_header(header, buffer, capacidade)**: reconstrói os campos a partir dos 98 bytes. Converte números, mas **não** faz toda a validação semântica; o chamador usa protocol_validate_header depois.
- **protocol_calculate_crc32(data, size)**: calcula CRC32 simples de um buffer, útil também em testes. Retorno zero para entrada inválida não é uma sinalização de erro separada.
- **protocol_validate_header(header)**: confere versão, código de mensagem conhecido e payload_size até MAX_PAYLOAD_SIZE. Não verifica o CRC nem implementa as mensagens futuras.
- **protocol_send_message(sock, message)**: valida mensagem, calcula CRC, envia header e depois payload usando network_send_all. Devolve PROTOCOL_OK ou PROTOCOL_ERROR. O payload passado continua pertencendo ao chamador.
- **protocol_receive_message(sock, message)**: lê header completo, rejeita versão/tipo/tamanho inválidos, aloca e lê payload completo, recalcula CRC e só então entrega header e buffer à mensagem. Retorna PROTOCOL_CLOSED se a conexão fechou antes de começar outro header; PROTOCOL_ERROR cobre truncamento ou CRC incorreto. O chamador precisa chamar message_free.

## 5. Payloads de transferência — transfer_protocol.c

Este arquivo define a **serialização do corpo** das mensagens C2; protocol.c serializa apenas o header externo e faz framing/CRC. Os encoders que usam uint8_t **output alocam um buffer**; o chamador chama free. Os decoders de chunk apontam data para dentro do payload recebido, sem copiá-lo: após message_free, esse ponteiro deixa de ser válido.

- **bounded_string_length(text, capacidade)**: procura o terminador NUL sem ler além da capacidade. Serve para validar nomes e IPs de tamanho fixo.

- **object_id_from_hex(id, text)**: aceita exatamente 64 dígitos hexadecimais e converte para os 32 bytes do ObjectID. É usado para distinguir um seletor por ID de um nome de arquivo.
- **document_wire_size(document)**: calcula o espaço necessário para operação, campos fixos e nome sem NUL.
- **encode_document_at(document, operation, output, capacity, used)**: valida nome, LZ4, contagem esperada de chunks e espaço; escreve operação, ObjectID, tamanho, contagem, compressão e nome. Informa bytes efetivamente usados.
- **decode_document_at(payload, size, expected_operation, document, used)**: faz a leitura inversa com checagens de tamanho, operação, nome, compressão e contagem de chunks. Rejeita NUL ou barra dentro do nome transmitido.
- **transfer_fill_transaction_id**: Compõe timestamp de nanossegundos com incremento lógico atômico, prefixo do NodeID e sequência atômica. Evita reutilização dentro da instância mesmo se o relógio retroceder. Respostas copiam o ID original.
- **transfer_encode_document / transfer_decode_document**: interface pública para o registro básico de documento, usado por BEGIN. O anúncio v2 e o resultado de lookup têm encoders próprios que acrescentam descritores/localizações. O decoder público exige consumir o payload inteiro, impedindo bytes extras ocultos.
- **transfer_encode_chunk / transfer_decode_chunk**: codificam e decodificam operação, ObjectID, índice, offset, tamanhos, SHA-256 do conteúdo original e bytes LZ4. Validam comprimentos básicos e limite de 4 MiB do dado original; a verificação criptográfica ocorre em storage.c ou file_client.c.
- **transfer_encode_object_operation / transfer_decode_object_operation**: formato curto de operação + ObjectID, usado no STORE/COMMIT.
- **transfer_encode_lookup_request(selector, ...)**: se o texto for um ObjectID hexadecimal válido, envia seletor binário por ID; caso contrário, envia nome. Aloca o payload.
- **transfer_decode_lookup_request(payload, ...)**: lê tipo e comprimento do seletor; valida comprimento, ausência de barra/NUL no nome e separa ObjectID ou nome.
- **transfer_lookup_result_free(result)**: libera listas de endpoints de cada chunk e o vetor de chunks; zera a estrutura. Deve ser chamado para um resultado de lookup montado ou decodificado.
- **transfer_encode_lookup_result(result, ...)**: serializa documento e, para cada chunk, descritor de 56 bytes, quantidade de Peers e seus NodeIDs, IPs e portas. Calcula tamanho e rejeita excesso antes de alocar.
- **transfer_decode_lookup_result(payload, ...)**: reconstrói documento e listas de endpoints; valida limites, portas e ausência de bytes sobrando. Em erro libera as partes já alocadas.
- **transfer_encode_chunk_request(id, index, ...)**: cria DOWNLOAD_REQ com operação, ObjectID e índice do chunk.
- **transfer_decode_chunk_request(payload, ...)**: exige tamanho e operação exatos e recupera ObjectID e índice.

## 6. Compressão e conteúdo — compression.c / content.c

**compression.c**

- **compression_lz4_bound(input_size, bound)**: consulta LZ4_compressBound para saber a capacidade mínima segura de saída. Rejeita tamanhos que não cabem na API int da LZ4.
- **compression_lz4_compress(input, input_size, output, output_size)**: aloca buffer com o limite calculado, chama LZ4_compress_default e devolve buffer/tamanho real. O chamador libera output com free.
- **compression_lz4_decompress(input, input_size, expected_size, output)**: aloca a saída, chama LZ4_decompress_safe e exige que o número de bytes reconstruídos seja exatamente expected_size. Isso ajuda a rejeitar chunks malformados; o chamador libera output.

**content.c**

- **content_sha256(data, size, digest)**: calcula SHA-256 de um buffer por libcrypto, usado nos chunks. Não calcula o ObjectID de arquivo inteiro; isso cabe a object_id_file.
- **content_validate_pdf**: Exige extensão .pdf sem distinção de maiúsculas e tenta abrir o arquivo. Não exige assinatura %PDF e não faz parse semântico. Isso admite os arquivos sintéticos do professor.
- **content_basename(path, output)**: extrai o último componente após / e valida se cabe em METADATA_NAME_SIZE. Esse nome é metadado; caminhos físicos internos usam ObjectID, não o nome recebido.

## 7. Identidade dos nós — node.c

- **normalize_ip(ip, normalized)**: auxiliar privado que tenta interpretar IPv4 e depois IPv6 por inet_pton; reescreve a forma textual normalizada com inet_ntop. NodeID depende dos bytes binários, não de diferenças de grafia do endereço.
- **node_generate_uuid(uuid)**: lê 16 bytes de /dev/urandom, repetindo leituras parciais e tratando EINTR. Ajusta os bits de versão/variante de UUID v4. Um UUID novo muda o NodeID mesmo com IP e porta iguais.
- **node_config_validate(config)**: exige porta diferente de zero, IP terminado dentro do buffer e endereço interpretável. Não testa conectividade.
- **node_config_init_with_uuid(config, ip, port, uuid)**: normaliza IP e preenche configuração usando UUID recebido. É essencial para reconstruir, no receptor, a mesma identidade anunciada no JOIN.
- **node_config_init(config, ip, port)**: gera UUID novo e chama a variante anterior. O próprio node.c não persiste UUID; app_identity, em app_config.c, fornece persistência ao Peer e ao Super Peer.
- **node_compute_id(config, id)**: calcula SHA-256 dos bytes binários do IP, porta em ordem de rede e UUID. PID e papel não entram no hash. Usa libcrypto.
- **node_init(node, config)**: calcula NodeID, copia configuração, registra getpid local e define papel inicial PEER. O PID reconstruído no servidor é local ao servidor, não o PID remoto.
- **node_validate(node)**: recalcula o NodeID, compara-o ao armazenado e verifica PID positivo e papel permitido. Verifica consistência interna da estrutura, não autentica uma máquina externa.
- **node_id_to_hex(id, output, output_size)**: escreve os 32 bytes como 64 dígitos hexadecimais mais NUL; exige buffer suficiente.
- **hexadecimal_value(character)**: auxiliar privado que converte um único dígito hexadecimal, aceitando letras maiúsculas/minúsculas.
- **node_id_from_hex(id, hex)**: exige 64 dígitos e reconstrói 32 bytes. Em erro a saída pode estar parcialmente preenchida; não a trate como ID válido.
- **node_id_compare(left, right)**: comparação lexicográfica normalizada em -1, 0 ou 1; também define ordem quando ponteiros são nulos. É um comparador, não uma eleição Bully.
- **node_id_equal(left, right)**: compara os 32 bytes; ponteiros nulos não contam como IDs iguais.
- **node_get_process_id(node)**: devolve o PID armazenado, ou -1 para ponteiro nulo.

## 8. Tabela de membros — membership.c; entrada exclusiva — superpeer.c

SuperPeer é opaco fora do .c. Internamente contém nó local, vetor dinâmico de membros, contagem/capacidade e mutex. O primeiro membro é o próprio Super Peer. Esta tabela de membership **não** é o índice de documentos de metadata.c.

- **main(argc, argv)**: é o ponto de entrada exclusivo de bin/superpeer e encaminha a execução a superpeer_run em superpeer_app.c. Não há seleção de papel por SUPERPEER_EXECUTABLE. A API local fica em membership.c, ligado aos testes sem superpeer.c.
- **lock_members(superpeer) / unlock_members(superpeer)**: traduzem códigos de erro de pthread para a convenção -1/errno. Devem cercar buscas e modificações do vetor compartilhado.
- **find_member_index_locked(superpeer, node_id, index)**: busca linear por NodeID. O sufixo locked avisa que o chamador já precisa possuir o mutex; quando index é não nulo, devolve a posição encontrada.
- **grow_members_locked(superpeer)**: dobra o vetor quando cheio, começando da capacidade padrão. Verifica overflow antes de realloc; em falha, o vetor antigo permanece válido.
- **set_member(member, node)**: copia os dados do nó e atualiza ALIVE/last_seen. last_seen é um registro local de atualização, **não** prova de heartbeat implementado.
- **superpeer_config_init_with_uuid(config, ip, port, uuid)**: monta uma configuração com UUID fornecido e capacidade inicial padrão. Usado quando se quer identidade reprodutível.
- **superpeer_config_init(config, ip, port)**: semelhante, mas gera UUID novo.
- **superpeer_create(config, output)**: valida configuração, inicializa nó local com papel SUPERPEER, aloca vetor/mutex e registra o próprio nó como membro 0. O chamador passa a ser dono do objeto resultante.
- **superpeer_destroy(superpeer)**: destrói mutex, vetor e objeto. Só chame depois de encerrar threads que ainda consultam membership.
- **superpeer_get_node(superpeer, output)**: copia o nó local para o chamador; não expõe ponteiro interno.
- **superpeer_register_node(superpeer, node)**: valida consistência do Node, busca por NodeID sob mutex e atualiza o membro existente ou acrescenta novo. Retorna ADDED, UPDATED ou REGISTER_ERROR; repetição do mesmo ID não aumenta contagem.
- **superpeer_unregister_node(superpeer, node_id)**: remove membro não local, compactando o vetor com memmove. Rejeita a remoção do próprio Super Peer. O handler de LEAVE identificado usa a remoção de membro e retira suas disponibilidades do índice antes do ACK; o comando legado anônimo de LEAVE não demonstra esse fluxo completo.
- **superpeer_find_member(superpeer, node_id, output)**: busca sob mutex e entrega uma **cópia** do membro. Evita deixar o cliente com ponteiro invalidado por realloc.
- **superpeer_member_count(superpeer)**: lê a quantidade sob mutex, incluindo o nó local. Zero pode significar erro, além de contagem zero.
- **superpeer_is_registered(superpeer, node_id)**: consulta existência sob mutex. Usada antes de aceitar ANNOUNCE. Zero representa ausência ou falha, distinguível por contexto/errno.

## 9. Índice de documentos — metadata.c

MetadataStore contém 257 buckets de hash, encadeamento para colisões e mutex. Documentos são indexados por ObjectID; disponibilidades de chunks são pares (índice, NodeID) alocados de forma esparsa. Esse índice é **volátil**: não possui persistência nem replicação.

- **fail(error)**: auxiliar que atribui errno e devolve -1; reduz repetição na API.
- **object_id_file(path, output, file_size)**: lê o arquivo em blocos de 64 KiB, alimenta SHA-256 incremental da libcrypto, soma tamanho com verificação de overflow e devolve digest/tamanho. Não carrega o PDF inteiro em memória; o chamador deve evitar alterações concorrentes no arquivo durante o cálculo.
- **object_id_to_hex(id, output, capacity)**: escreve 64 dígitos hexadecimais e terminador.
- **bucket(id)**: calcula índice de bucket a partir de todos os bytes do ObjectID. Possíveis colisões são resolvidas depois pela comparação completa do ID.
- **lock_store(store)**: adquire o mutex e traduz eventual código de pthread para errno.
- **find_entry(store, id)**: percorre a lista no bucket e devolve o endereço do ponteiro da entrada; simplifica inserção/remoção. Exige mutex adquirido.
- **free_entry(entry)**: libera a lista de disponibilidades, o vetor de descritores e a entrada do documento.
- **metadata_create(output)**: aloca store zerado, inicializa mutex e devolve ponteiro opaco.
- **metadata_destroy(store)**: libera todas as entradas e o mutex; exige que outras threads já tenham parado de usá-lo.
- **metadata_register_document(store, id, name, file_size)**: cria documento e calcula chunk_count, ou aceita repetição do mesmo ID/tamanho. Se outro nome vier para o mesmo ID, o primeiro nome fica preservado; tamanho diferente gera EEXIST.
- **metadata_find_document(store, id, output)**: procura pelo ObjectID e devolve cópia de MetadataDocument. ENOENT quando não existe.
- **metadata_remove_document(store, id)**: remove documento e todas as suas associações de chunk; é uma operação da API local, não um fluxo de exclusão de documento distribuído.
- **change_chunk(store, id, index, peer, remove)**: auxiliar comum que valida documento/índice, busca a associação específica e insere ou remove sob o mesmo mutex. Inserção repetida é idempotente; remoção ausente gera ENOENT.
- **metadata_register_chunk / metadata_unregister_chunk**: wrappers que chamam change_chunk com modo inserir/remover. Não verificam se o NodeID está registrado em membership; essa responsabilidade pertence à integração.
- **metadata_chunk_peers(store, id, chunk_index, output, count)**: conta e copia todos os NodeIDs que anunciam aquele chunk. O chamador libera a matriz retornada com free; sem localizações, devolve NULL e contagem zero.

## 10. Ponte entre índice e membros — directory.c

Directory guarda ponteiros para MetadataStore e SuperPeer; **não é dono deles**. Não mantém índice de nomes separado: a resolução de nome pertence a metadata_find_name.

- **directory_create**: Aloca adaptador leve contendo ponteiros emprestados ao índice e à tabela de membros; não mantém outra lista de nomes.
- **directory_destroy**: Libera somente o adaptador; metadados e membership pertencem ao serviço.
- **directory_announce**: Valida compressão/extensão e adapta TransferDocument para MetadataDocument; chama metadata_announce com descritores e dono. Não cadastra chunks um a um.

- **directory_lookup**: Resolve nome na hash table, copia documento/descritores/localizações e traduz NodeIDs para endpoints com membership. ENOTUNIQ indica ambiguidade; ENODATA indica chunk sem localização ativa.

## 11. Persistência local do Peer — storage.c

Storage administra .peer_storage/<porta>/pending e objects. Cada objeto tem pasta nomeada pelo ObjectID hexadecimal, um manifest.bin versionado e arquivos chunk-<índice>.lz4. O manifest armazena metadados, dono, timestamp, estado e descritores. O mutex do Storage protege a lista; cada documento tem seu próprio mutex para operações demoradas. Estado publicado é atômico. Nomes enviados por usuários **não** são usados para montar os caminhos físicos.


- **write_all_fd(fd, data, size)**: repete write até gravar tudo, tratando EINTR e escrita parcial.
- **read_all_fd(fd, data, size)**: repete read até completar. EOF prematuro torna o manifest ou chunk inválido.
- **ensure_directory_tree(path)**: cria componentes do caminho com permissão 0700, tolerando EEXIST. Usado para raiz, pending e objects.
- **object_hex(id, output)**: wrapper de object_id_to_hex para obter nome seguro do diretório.
- **document_directory(storage, id, final, output)**: constrói caminho de pending ou objects para um ObjectID, validando o comprimento.
- **chunk_path(storage, id, index, final, output)**: acrescenta o nome determinístico do chunk ao diretório do objeto.
- **find_document(storage, id)**: busca linear no catálogo em memória; usada sob o mutex do Storage.
- **write_manifest(storage, stored, final)**: serializa magic, versão, documento, proprietário, estado e cada descritor para arquivo temporário; executa fsync e rename para publicar o manifest. Em falha remove o temporário.
- **read_manifest(storage, path, final, output)**: lê/valida magic, versão, tamanhos, nome, estado e descritores; compara tamanhos reais dos chunks no disco. Para objeto finalizado, chama verify_document, que recalcula os hashes. Só devolve catálogo alocado quando tudo é coerente.
- **load_documents(storage, final)**: percorre pending ou objects, lê manifests válidos, confere se o nome da pasta corresponde ao ObjectID e evita duplicatas. Recupera objetos finalizados deixados em VERIFYING após publicação.
- **storage_create(root, owner, output)**: aloca Storage/mutex, cria as pastas e carrega primeiro os objetos finalizados, depois os pendentes. O owner é o NodeID deste Peer.
- **storage_destroy(storage)**: libera catálogo e mutex; não apaga os arquivos persistidos. É o motivo de documentos poderem ser reencontrados após reinicialização.
- **storage_begin(storage, document)**: abre upload lógico. Se ObjectID e tamanho/contagem já existem, aceita repetição compatível; senão, cria registro CREATED, vetor de chunks e manifest em pending. Não recebe os bytes do arquivo nesta etapa.
- **storage_put_chunk**: Valida índice/offset/tamanhos e hash após LZ4; grava chunk temporário, fsync/rename e manifest. Usa lock por documento. Se persistir o manifest falhar, restaura o descritor/estado anterior em memória para não devolver sucesso falso na repetição.
- **verify_document**: Reabre e descomprime chunks em ordem, verifica hashes individuais e alimenta EVP_DigestUpdate. Ao final compara tamanho e ObjectID. Não cria cópia temporária integral do documento.
- **storage_commit**: Exige todos os chunks e verifica integridade sob lock do documento. Grava manifest final em pending, renomeia diretório e sincroniza pais antes de confirmar FINISHED em memória. Falha mantém erro; repetição compatível é aceita.
- **storage_find(storage, type, id, name, document)**: consulta apenas documentos FINISHED por ID ou nome; nome ambíguo para IDs diferentes gera ENOTUNIQ.
- **storage_read_chunk(storage, id, index, chunk, owned_data)**: lê chunk comprimido de objeto finalizado e devolve descritor + buffer de bytes. O chamador é dono de owned_data e deve usar free; chunk.data aponta para o mesmo buffer.
- **storage_list(storage, documents, count)**: copia os documentos FINISHED do catálogo. O Peer usa a lista ao reiniciar para reanunciá-los; o chamador libera a matriz com free.

O manifest e o índice do Super Peer têm papéis diferentes: o primeiro permite ao Peer recuperar seus bytes locais; o segundo informa ao cliente **qual Peer** possui cada chunk. O Super Peer não guarda o conteúdo do PDF.

## 12. Servidor TCP concorrente — concurrent_server.c

- **worker_run**: aguarda a condição enquanto fila está vazia; retira um descritor sob mutex, chama o handler fora do lock e fecha a conexão após o atendimento. A posição active permite interromper sockets no encerramento.
- **concurrent_server_create**: valida argumentos e inicializa mutex, condição e 32 workers. Se uma criação falhar, acorda/aguarda os já criados e devolve erro sem assumir ownership do listener.
- **concurrent_server_run**: executa accept e insere na fila de até 64 sockets. Se cheia, fecha a conexão excedente. Ao sair solicita parada.
- **concurrent_server_stop**: marca stopping, faz shutdown no listener e nas conexões ativas, fecha pendentes e acorda workers. É idempotente.
- **concurrent_server_destroy**: solicita parada, aguarda todos os workers, fecha listener, destrói sincronização e libera memória. O serviço deve aguardar o laço accept antes de destruir.

## 13. Chamada TCP simples — rpc.c

- **rpc_encode_join_payload(config, output)**: valida NodeConfig e monta os 64 bytes do descritor JOIN: IP textual/padding, porta em ordem de rede e UUID. Essa representação não inclui PID nem envia a struct C crua.
- **rpc_call**: Recebe origem/destino explícitos quando conhecidos, abre TCP, envia frame e verifica TransactionID, destino da resposta e origem remota esperada. Fecha a conexão em todos os caminhos. Payload recebido pertence ao chamador.

## 14. Servidor de armazenamento — peer_service.c

PeerService combina Node, Storage, runtime concorrente e endereço do Super Peer. Uma conexão atendida pelo Peer processa uma requisição e devolve uma resposta; os workers de upload/download abrem suas próprias conexões.

- **stop_service(signal_number)**: handler de sinal que faz shutdown no socket de escuta para acordar o accept e iniciar a saída do serviço.

- **send_response(service, socket_fd, request, type, payload, payload_size)**: constrói resposta com source_node local, destination_node da requisição e **o mesmo TransactionID**. Copia o payload fornecido, envia pelo protocolo e libera a cópia.
- **join_superpeer**: Envia JOIN com identidade local, exige ACK, decodifica descritor remoto e recalcula NodeID. Só guarda o ID do SP se corresponder ao source_node da resposta.
- **announce_document**: Obtém cópia dos descritores finalizados de storage, codifica anúncio v2 e exige ACK do SP. Libera descritores/payload e preserva erro remoto específico.
- **announce_catalog(service)**: obtém storage_list e chama announce_document para cada objeto FINISHED. Reconstitui o índice volátil do Super Peer quando o Peer reinicia.
- **handle_store(service, socket_fd, message)**: despacha STORE/BEGIN para storage_begin, STORE/CHUNK para storage_put_chunk e STORE/COMMIT para storage_commit seguido de announce_document. Responde ACK apenas se a operação inteira deu certo; nos demais casos, ERROR.
- **handle_download(service, socket_fd, message)**: decodifica ObjectID/índice, lê chunk comprimido finalizado, codifica DOWNLOAD_REP e o envia. Libera payload codificado e o buffer que veio de storage_read_chunk.
- **serve_connection(context, socket_fd)**: callback da conexão aceita. Recebe uma Message e atende STORE, DOWNLOAD_REQ ou PING textual; qualquer outro tipo recebe ERROR. Libera a mensagem recebida.
- **initialize_service**: Escolhe diretório configurado ou padrão da porta, chama app_identity e node_init e carrega storage. Não anuncia um endpoint antes de criar o listener.
- **peer_service_run**: Inicializa storage e identidade, prepara listener antes de JOIN, reanuncia catálogo, inicia controle local e atende TCP. No fim interrompe controle, envia LEAVE somente se efetivamente entrou no SP, aguarda runtime e destrói storage.

## 15. Cliente de arquivo — file_client.c

Há dois contextos de trabalho: UploadWork e DownloadWork. Ambos distribuem índices de chunks por mutex, guardam o primeiro erro e usam várias threads. Cada RPC abre conexão TCP própria. A quantidade de workers padrão é mínimo entre CPUs, chunks e 8; PEER_TRANSFER_THREADS aceita de 1 a 32.

- **transfer_worker_count(chunk_count)**: lê número de CPUs e eventual variável PEER_TRANSFER_THREADS; limita a quantidade ao número de chunks e impede zero workers.
- **request_expect**: Chama rpc_call com sessão e destino esperados, confere o tipo da resposta e traduz ERROR wire para errno. Libera resposta em erro.
- **pread_all(fd, buffer, size, offset)**: lê um chunk inteiro em posição explícita do arquivo sem compartilhar o offset do descritor entre threads. Repete leituras parciais e EINTR.
- **pwrite_all(fd, buffer, size, offset)**: grava um chunk inteiro no offset correto do destino, também seguro contra interferência entre offsets de workers distintos. Repete escritas parciais e EINTR.
- **upload_fail(work, error)**: registra, sob mutex, somente o primeiro erro de upload. Workers posteriores param de pegar novos índices.
- **upload_worker(argument)**: pega próximo índice, lê bytes originais por pread_all, calcula SHA-256 do chunk, comprime com LZ4, codifica STORE/CHUNK e exige ACK do Peer. Atualiza total comprimido e hashes de saída; libera buffers temporários.
- **execute_upload_workers(work, worker_count)**: cria as threads de upload, aguarda todas com pthread_join e propaga o primeiro erro registrado. Mesmo após falha, não abandona threads ativas.
- **file_client_upload**: Recebe FileSession do Peer ativo e FILE de progresso; valida extensão, calcula ObjectID incremental, descobre destino por PING e envia BEGIN, chunks paralelos e COMMIT. Só confirma upload após ACK que inclui anúncio ao SP.
- **download_fail(work, error)**: equivalente de upload_fail para os workers de download.
- **download_one(work, index)**: pede o chunk aos endpoints anunciados, um por vez. Para cada resposta, confere descritor, offset/tamanho, descomprime LZ4, valida SHA-256 e usa pwrite_all; se um endpoint falha, tenta o próximo.
- **download_worker(argument)**: distribui índices aos workers até acabar a fila ou ocorrer o primeiro erro. Chama download_one para cada chunk obtido.
- **execute_download_workers(work, worker_count)**: cria/aguarda threads e devolve o primeiro erro da operação.
- **lookup_document**: Descobre identidade do SP por PING, envia LOOKUP com a identidade do serviço e decodifica metadados/localizações v2. Retorna resultado alocado para posterior liberação.
- **file_client_download**: Recebe a mesma sessão do Peer ativo; consulta o SP, cria .part exclusivo, busca chunks em paralelo e valida hashes contra o índice. Verifica ObjectID e publica por link exclusivo/fsync, sem sobrescrever destino.
- **file_client_benchmark_lz4(path)**: percorre um PDF em chunks e mede apenas a compressão LZ4; informa bytes, duração e taxa. Não é teste rígido de desempenho nem verifica rede.

## 16. Entrada do Peer e comandos — peer.c

- **parse_port(text, port)**: converte porta decimal válida de 1 a 65535, rejeitando texto extra.
- **message_name(type)**: devolve rótulo legível para os comandos legados PING/JOIN/LEAVE e respostas.
- **legacy_command(command, host, port)**: executa uma operação de compatibilidade do C1. PING envia quatro bytes e espera PONG; JOIN monta NodeID/descritor de teste e espera ACK; LEAVE espera ACK. Esse LEAVE não demonstra remoção real do membro.
- **option_command(argc, argv)**: interpreta pares de opções --cmd, --host, --port, --file e --output. Para upload/download chama local_control_command; para ping/join/leave chama legacy_command. Upload/download admitem host/porta padrão; os comandos legados exigem endpoint informado.
- **usage(program)**: imprime todas as formas aceitas da linha de comando.
- **main(argc, argv)**: ponto de entrada de bin/peer e bin/client. Ignora SIGPIPE, escolhe serve, upload, download, benchmark ou comando legado; usa 127.0.0.1:55101 como Super Peer e 127.0.0.1:55102 como Peer de armazenamento quando o comando posicional omite endereço/porta. Retorna EXIT_FAILURE em erro.

## 17. Atendimento do Super Peer — superpeer_app.c

PeerContext neste arquivo é o estado do **processo Super Peer**, apesar do nome histórico. Ele reúne identidade local, membership, índice MetadataStore, Directory, socket e runtime concorrente.

- **handle_signal(signal_number)**: limpa a flag sig_atomic_t que mantém o processo vivo; a saída normal para o runtime é feita depois, fora do handler.
- **parse_port(text, port)**: valida porta decimal no intervalo permitido.
- **parse_command_type(text, type)**: converte ping, join ou leave para o código de mensagem correspondente.
- **parse_node_arguments(argc, argv, arguments)**: aceita forma posicional legada e opções getopt_long. Distingue servidor e comando; app_config_load já interpretou o arquivo e as opções comuns antes dessa chamada.
- **print_node_id(node_id)**: imprime os 32 bytes como 64 dígitos hexadecimais.
- **message_type_name(type)**: devolve nome textual para logs TX/RX dos comandos C1.
- **set_text_payload(message, text)**: aloca e copia payload textual sem o NUL final; usado para PING. A mensagem passa a ser dona do buffer.
- **message_payload_equals(message, text)**: compara comprimento e bytes de um payload com texto esperado, sem exigir terminador NUL na rede.
- **node_id_is_zero(node_id)**: verifica se os 32 bytes são zero. JOIN inicial pode não conhecer o ID do destinatário e usar destino zerado.

- **encode_join_payload_alloc**: Aloca 64 bytes e delega serialização a rpc_encode_join_payload; libera em erro. Evita outro encoder JOIN.

- **initialize_local_identity(peer, local_port)**: cria Node local e SuperPeer membership com **o mesmo UUID**, garantindo que os dois objetos tenham o mesmo NodeID.
- **send_reply(peer, client_fd, request, type, payload, payload_size, include_node_descriptor)**: constrói resposta com NodeID local, destino da requisição, mesmo TransactionID e payload opcional; pode anexar o próprio descritor de nó em ACK de JOIN.
- **register_join(peer, message)**: valida destino, decodifica descritor, recalcula NodeID do remetente e compara ao header; rejeita autorregistro e inclui/atualiza membro antes do ACK.
- **register_announcement(peer, message)**: exige que source_node já esteja cadastrado, decodifica STORE/ANNOUNCE e delega registro de documento/chunks a directory_announce.
- **answer_lookup(peer, client_fd, message)**: decodifica seletor, consulta Directory, serializa metadados/localizações e responde DOWNLOAD_REP; em erro envia ERROR. Libera estruturas temporárias.
- **handle_client(context, client_fd)**: callback para conexão TCP. Usa getpeername para obter ip_origem real da conexão, recebe mensagens em laço, registra log e despacha JOIN, PING, LEAVE, ANNOUNCE e LOOKUP; mensagens futuras/inesperadas recebem ERROR. **origem** no log é o NodeID do header (exceto PING, em que é omitido), não o IP. LEAVE remove membro e disponibilidades antes do ACK.
- **accept_clients**: Executa o laço do runtime que distribui conexões ao pool limitado; não cria threads ilimitadas por cliente.
- **connect_and_join(peer, ip, remote_port)**: forma legada de conectar este servidor a outro nó, enviar JOIN e verificar ACK, TransactionID e identidade recebida; também registra o remoto em sua tabela local.
- **execute_command(arguments)**: modo --cmd de bin/superpeer/bin/node para PING, JOIN ou LEAVE. Monta mensagem, envia uma vez, confere resposta e encerra.

- **print_usage(program_name)**: mostra os modos aceitos pelo Super Peer, incluindo interface posicional legada.
- **superpeer_run(argc, argv)**: inicializa modo servidor ou executa comando único; no modo servidor cria identidade, MetadataStore, Directory e socket, inicia atendimento e aguarda sinal. No encerramento para conexões e libera recursos na ordem inversa. É chamado pelo main exclusivo de superpeer.c.

## 18. Arquivos fora do build

`client.c` foi removido por duplicar a CLI e não participar do build. O alias `bin/client` continua funcionando. `teste.c` é uma demonstração isolada, preservada, não usada pelos executáveis do trabalho.

## 19. Testes C: função por função

**test_node_superpeer.c** executa a API local de identidade/membership sem sockets.

- **test_node_id_is_deterministic**: usa IP, porta e UUID fixos e compara NodeID com hash conhecido.
- **test_node_records_process_and_round_trips_id**: verifica PID/papel, IPv6, conversão hexadecimal reversível, comparação de IDs e erro de buffer pequeno.
- **test_node_rejects_invalid_configuration**: exercita IP inválido, porta zero, IP sem NUL e UUID gerado.
- **test_superpeer_registers_and_finds_members**: confere autorregistro, crescimento da tabela, inclusão, consulta e atualização sem duplicata.
- **test_superpeer_rejects_inconsistent_nodes_and_unregisters**: testa ID adulterado, proteção do nó local, remoção de membro e ausência.
- **register_members**: corpo executado por cada thread do teste de concorrência; registra 25 nós diferentes.
- **test_superpeer_members_are_thread_safe**: cria quatro threads de registro, espera todas e confere 101 membros incluindo o local. Exercita sincronização, mas não é prova absoluta de ausência de corridas.
- **main**: chama os seis cenários e imprime sucesso apenas se nenhum assert falhar.

**tests/c1/test_protocol.c** testa serialização/framing sem depender de porta TCP externa.


- **test_message_round_trip**: cria socketpair local, envia PING com header/payload e compara a mensagem recebida; verifica liberação dos recursos.
- **test_crc32_reference_vector**: confere CRC32 do vetor conhecido “123456789”.
- **main**: executa os cenários e anuncia sucesso.

**tests/c2/test_metadata.c** é um teste concentrado em um main longo.

- **register_peer**: função de thread que acrescenta um NodeID à disponibilidade do chunk 0.
- **main**: cria arquivos temporários e confere SHA-256 conhecido, leitura incremental, colisões de hash table, deduplicação, disponibilidade esparsa, cópias de saída, fronteiras de tamanho, erros e oito registros concorrentes. Depois libera os recursos.

**tests/c2/test_transfer_negative.c** procura rejeições, não apenas caminho feliz.

- **make_wire_message**: produz uma mensagem PING válida em bytes para que outros testes a adulterem.
- **test_protocol_rejections**: modifica CRC, trunca payload e aumenta tamanho anunciado além do limite; espera PROTOCOL_ERROR.
- **cleanup_storage**: remove apenas os arquivos/diretórios temporários específicos criados por estes testes.
- **test_storage_validation**: verifica que SHA de chunk inválido, índice/offset incorretos e duplicata incompatível são recusados.
- **test_object_id_mismatch**: usa chunks individualmente válidos, mas ObjectID final adulterado; COMMIT precisa falhar.
- **test_pending_restart**: deixa upload incompleto, destrói/recria Storage, retoma o primeiro chunk e grava o seguinte; verifica recuperação do manifest pending.
- **main**: executa esses quatro cenários e imprime sucesso se todos os asserts passarem.

## 20. Scripts e Makefile

Scripts Bash não têm protótipos C. As funções abaixo são auxiliares do próprio script e só são chamadas em sua execução.

**script_testes.sh** — suíte C1 baseada em bin/node/bin/client:

- **ok / notok**: incrementam contadores e imprimem PASS/FAIL.
- **cleanup**: encerra o processo de servidor iniciado pelo script; é registrado em trap.
- **corpo principal**: compila teste de protocolo, inicia servidor, envia PING/JOIN/LEAVE, testa concorrência, framing, versão inválida e identificação. O script contabiliza dez verificações. Uma contagem prevista não substitui o resultado de uma execução.

**script_testes_peer.sh** — teste legado de bin/node atuando como nó em ambos os lados:

- **ok / notok**: imprimem/contam resultados.
- **cleanup**: encerra os processos iniciados por start_peer e os aguarda.
- **wait_for_log**: espera até certo limite uma expressão aparecer em log; evita depender apenas de sleep fixo.
- **start_peer**: inicia processo legado com saída direcionada ao log e guarda PID.
- **corpo principal**: PING concorrente, JOIN entre nós e validação de logs. O nome do script é histórico; ele usa o alias bin/node, não o servidor de chunks de peer_service.c.

**script_testes_c2.sh** — suíte principal do upload/download C2:

- **pass / fail**: registram resultados.
- **stop_pid / cleanup**: encerram Super Peer e Peers criados; cleanup remove a pasta temporária específica no sucesso e preserva as evidências quando houve falha.
- **wait_port**: espera uma porta TCP responder antes de começar dependentes.
- **corpo principal**: compila com -Werror, executa testes negativos, inicia dois Peers, envia PDF pequeno e multichunk, verifica benchmark, download por nome/ID, fallback, reanúncio, idempotência e erros de entrada/destino.

**redvidassobretrabalhodepd/env.sh** define PROJECT_ROOT, caminhos dos binários, dados, configuração e logs. Não define função; fornece variáveis para common.sh e run.sh. **redvidassobretrabalhodepd/common.sh** define:

- **log / section**: exibem mensagens informativas e separadores.
- **ok / fail**: contabilizam aprovações e falhas.
- **cleanup**: envia sinal aos PIDs guardados no vetor PIDS; registrado em trap.
- **require_bin**: verifica se o executável existe e tem permissão de execução.
- **wait_for_pattern**: aguarda uma expressão aparecer em arquivo de log até timeout.
- **assert_file_equal**: compara bytes de dois arquivos com cmp.
- **assert_contains**: procura expressão no log.
- **assert_no_crash**: procura textos típicos de crash/deadlock no log; não substitui sanitizers.
- **summary**: imprime contadores e retorna sucesso somente com FAIL igual a zero.

**redvidassobretrabalhodepd/run.sh** importa common.sh, extrai fixtures quando necessário, escolhe arquivos .pdf sem exigir assinatura, inicia SP e Peer, seleciona o executor local, faz upload/download e compara bytes. Os três PDFs sintéticos participam do teste. A versão previamente adaptada foi preservada em run.pre-refactor.sh.

**Makefile** não define funções C: suas regras ligam os módulos aos executáveis, criam aliases bin/node e bin/client, e expõem alvos como all, test, test-aluno2 e test-c2. Os .h são dependências para recompilação; a presença de um .c no diretório não significa que ele participa de um binário — confira a receita do alvo.

## 21. Roteiros de leitura para entender de fato

Para compreender uma requisição sem se perder, siga primeiro esta cadeia:

1. **Comando upload:** peer.c main → local_control_command → socket Unix → handle_command → file_client_upload → rpc_call → network_connect → protocol_send_message.
2. **Recepção no Peer:** concurrent_server_run → peer_service.c serve_connection → handle_store → storage_begin/storage_put_chunk/storage_commit.
3. **Anúncio:** peer_service.c announce_document → rpc_call → superpeer_app.c handle_client → register_announcement → directory_announce → metadata_announce (publicação conjunta de documento, descritores e localizações).
4. **Download:** file_client_download → lookup_document → superpeer_app.c answer_lookup → directory_lookup → download_worker/download_one → peer_service.c handle_download → storage_read_chunk.
5. **Integridade:** CRC32 verifica a mensagem em trânsito; SHA-256 de cada chunk verifica seus bytes descomprimidos; o ObjectID verifica o documento inteiro. Essas três verificações não são intercambiáveis.

Ao estudar qualquer função, pergunte: **quem a chama?**, **quem é dono de cada buffer?**, **qual mutex protege o estado?**, **qual erro pode retornar?**, **em que ponto o dado passa a ser visível?** Esse conjunto de perguntas costuma revelar mais que ler linha por linha sem o fluxo em mente.

## 22. Limites reais da implementação

- Embora node.c aceite IPv6 para identidade, a camada network.c conecta/escuta apenas IPv4; não prometa operação IPv6 ponta a ponta.
- PDF é aceito pela extensão case-insensitive; não existe validação semântica do formato.
- LEAVE voluntário remove membro e disponibilidade; crash depende de fallback, sem detector automático.
- MetadataStore do Super Peer é volátil; a recuperação depende de Peers reiniciarem e reanunciarem seus manifests.
- Não há autenticação criptográfica do remetente, TLS, DHT/Chord, Gossip, eleição, replicação automática, SMR, 2PC, LFU ou IST neste checkpoint. Um NodeID declarado e um IP observado são informações diferentes.
- Disponibilidade, consistência global e taxas de desempenho futuras não devem ser apresentadas como garantidas apenas porque a estrutura de código reserva nomes ou estados para elas.

## 23. Funções e contratos introduzidos na refatoração

### app_config.c

- **assign**: mapeia chave para campo de configuração, verifica comprimento/IPv4/porta e rejeita chave desconhecida. Não permite resolver DNS silenciosamente.
- **trim**: elimina espaços antes/depois da linha ou valor, modificando o buffer local de parsing.
- **app_config_load**: define padrões, lê arquivos chave=valor, aplica overrides e remove opções comuns de argv antes da CLI específica. Publica bind em ambiente antes de criar threads.
- **app_identity**: cria diretórios, cria UUID com exclusividade ou lê exatamente 16 bytes, sincroniza a criação e monta NodeConfig. Usa O_NOFOLLOW no arquivo UUID. NodeID é calculado por node_init, não pela leitura do arquivo.

### local_control.c

- **control_path**: constrói diretório privado por UID e verifica tipo, dono e permissões antes de usar o socket da porta.
- **send_string / receive_string**: transmitem uint32 comprimento seguido dos bytes. Receive aloca terminador local, rejeita NUL interno e limita tamanho; o chamador libera.
- **progress_write**: callback de FILE criado com fopencookie; transforma escrita em frame de progresso UINT32_MAX mais comprimento/dados. Não redireciona stdout global.
- **handle_command**: recebe operação, porta e cinco strings; cria sessão com NodeID do serviço e stream de progresso; chama upload/download; envia resultado final e libera todos os campos.
- **control_loop**: aceita um comando local por vez, registra descritor ativo sob mutex, atende e fecha. Transferências internas continuam paralelas.
- **local_control_start**: adquire flock exclusivo, remove somente socket antigo protegido pelo lock, cria AF_UNIX com permissão 0600 e inicia a thread de controle.
- **local_control_stop**: sinaliza stopping, interrompe sockets local/listener, aguarda thread e remove socket. Não apaga storage.
- **absolute_path**: combina caminho relativo com cwd da CLI sem alterar cwd do serviço.
- **local_control_command**: conecta ao Peer escolhido, envia campos e imprime frames até o resultado final. Aguarda progresso sem prazo total arbitrário; fechamento do serviço encerra a espera. Não cria conexão TCP ao SP.

### wire.h e remote_error.h

- **wire_put_u16/u32/u64**: escrevem números por bytes mais significativos primeiro; nenhum cast de struct é transmitido.
- **wire_get_u16/u32/u64**: reconstroem números usando shifts em tipos unsigned de largura suficiente.
- **remote_error_encode**: converte errno de domínio para versão/código estáveis na rede.
- **remote_error_decode**: traduz código wire para erro local; payload desconhecido vira EREMOTEIO.

### Rede e identificação

- **timeout_ms**: lê timeout em segundos (1–3600) e usa padrão se inválido.
- **now_ms**: lê CLOCK_MONOTONIC em milissegundos.
- **wait_ready**: usa poll até readiness/deadline, retomando EINTR sem reiniciar o prazo.
- **rpc_decode_join_payload**: exige tamanho de JOIN, IP terminado e padding zero; converte porta e valida NodeConfig.
- **discover_peer**: envia PING com sessão do serviço, exige PONG e NodeID não nulo; usa esse destino nas operações seguintes.
- **execute_workers**: gerencia criação/join dos workers de uma operação; o callback de falha interrompe distribuição de novas tarefas. Wrappers execute_upload_workers/execute_download_workers propagam o primeiro erro preservado.
- **content_pdf_name**: verifica apenas extensão .pdf case-insensitive, sem acessar disco.
- **content_sync_parent**: abre e sincroniza diretório pai após publicação de entrada de arquivo.

### Metadados

- **metadata_announce**: valida descritores e cria candidato completo antes de lock/publicação. Dentro da seção crítica confere conflitos, preserva dono/nome original, combina localizações e troca a entrada apenas no sucesso.
- **metadata_find_name**: percorre os buckets sob mutex; devolve ObjectID único, ENOENT ou ENOTUNIQ.
- **metadata_chunk_descriptor**: devolve cópia consistente do descritor, sem ponteiros emprestados.
- **metadata_remove_peer**: remove todas as associações do NodeID; preserva documentos e descritores para eventual novo anúncio.
- **encode_descriptor / decode_descriptor**: convertem registro de 56 bytes sem depender do padding de MetadataChunk.
- **transfer_encode_announcement / transfer_decode_announcement**: manipulam anúncio v2, com documento e lista completa de descritores. Rejeitam excesso/truncamento antes de disponibilizar saída.

### Armazenamento

- **allocate_document / release_document**: inicializam/destroem o mutex individual do registro. O vetor de chunks é liberado pelo chamador antes de release.
- **sync_parent**: sincroniza diretório pai de manifest/chunk ou pasta publicada.
- **storage_descriptors**: consulta catálogo, adquire lock do documento e devolve cópia dos descritores somente se FINISHED. Essa cópia é usada no anúncio, não inclui conteúdo do PDF.

### Testes adicionais

- **test_storage_atomic.c/main**: cria obstáculo real no caminho temporário do manifest, exige falha sem confirmação, remove o obstáculo, repete e verifica recuperação. Também testa chunk bem formado mas incompatível.
- **tests/c2/integration.py**: orquestra processos em diretório temporário, serializa frames independentemente e confere 46 propriedades, incluindo origem, concorrência observada, timeout, falha de bind e reconstrução do SP.

## 24. Referência vigente

Consulte [refatoracao_checkpoint_2.md](refatoracao_checkpoint_2.md) para comandos, evidências e limitações. Os algoritmos de checkpoints posteriores não foram implementados. PDF é uma política de extensão neste checkpoint, não uma validação semântica. Os dois executáveis agora usam headers oficiais das bibliotecas.


## 25. Entendendo os mecanismos antes de abrir o catálogo

### 25.1 Quem executa upload e quem possui o arquivo?

Há dois processos envolvidos no comando local: a CLI curta, que recebe seus argumentos, e o Peer de longa duração, que já fez JOIN. A CLI não pega emprestado o NodeID para se passar pelo serviço: envia um comando pelo socket Unix. O serviço cria uma FileSession com seu NodeID, um FILE de progresso e o diretório de trabalho informado pela CLI. É esse serviço que abre o arquivo, calcula hashes e estabelece as conexões TCP.

Por isso, “host/porta remotos” e “porta do Peer local executor” são coisas diferentes. Um Peer B pode receber o comando de upload e enviar os chunks a A. No download, B consulta o Super Peer e obtém os bytes de A. file_client.c é uma biblioteca de operações, não um terceiro executável de cliente independente.

### 25.2 Memória: ponteiro não é conteúdo

Considere um chunk com 4 MiB. TransferChunk guarda índice, offset, tamanhos, hash e um ponteiro data; copiar essa struct não copia 4 MiB. O decoder de chunk faz data apontar para uma região do payload da Message recebida. Esse endereço só permanece válido enquanto o payload existir. storage_read_chunk, ao contrário, aloca bytes lidos do disco e os devolve também em owned_data, para tornar explícito quem chama free.

Um parâmetro T **output permite mudar o ponteiro da variável do chamador. Um T *output permite preencher um objeto que já existe. Nenhum dos dois, isoladamente, informa ownership: confira se a função aloca memória e qual rotina deve destruí-la.

| Resultado | Quem libera ou fecha |
| --- | --- |
| Message recebida | Chamador, com message_free |
| Payload produzido por encoder | Chamador, com free |
| TransferLookupResult | Chamador, com transfer_lookup_result_free |
| Buffer LZ4 comprimido/descomprimido | Chamador, com free |
| Vetor de peers/descritores/documentos copiado | Chamador, com free |
| Socket devolvido por connect/accept | Chamador ou runtime que assumiu seu atendimento |
| Storage/MetadataStore/SuperPeer | Serviço, após encerrar seus usuários |
| data de transfer_decode_chunk | Ninguém separadamente: pertence ao payload original |
| Directory | Libera o adaptador, não os objetos apontados por ele |

### 25.3 Por que dois laços para enviar e receber?

Se você pede send de 1000 bytes e recebe 300, ainda faltam 700. O código avança o ponteiro por done e reduz o tamanho restante. ssize_t é importante porque também representa -1. Converter -1 para size_t antes da checagem produziria um inteiro positivo enorme.

EINTR pede nova tentativa após interrupção por sinal. EAGAIN/EWOULDBLOCK indica que a operação não bloqueante ainda não pode avançar: poll espera prontidão até o deadline. A cada transferência positiva, o prazo de inatividade é renovado; não existe aqui um cronômetro único que obrigue um PDF grande a terminar em 30 segundos.

recv igual a zero significa encerramento ordenado pelo remoto. network_recv_exact devolve a quantidade acumulada; protocol_receive_message decide se isso foi fechamento entre frames ou truncamento no meio de um frame. send_all falha com -1 se não conseguir terminar, mesmo que uma parte já tenha sido transmitida.

### 25.4 Framing não é serialização, CRC não é SHA

Framing identifica onde uma mensagem acaba no fluxo TCP. Serialização define a representação de cada campo. O programa transmite 98 bytes de header e depois payload_size bytes; não transmite sizeof(Header), pois o compilador pode inserir padding.

| Campo do header | Offset inicial | Bytes |
| --- | ---: | ---: |
| versão | 0 | 1 |
| tipo | 1 | 1 |
| origem | 2 | 32 |
| destino | 34 | 32 |
| TransactionID | 66 | 16 |
| timestamp | 82 | 8 |
| tamanho do payload | 90 | 4 |
| CRC32 | 94 | 4 |

CRC32 detecta alteração nos bytes serializados do header/corpo; não autentica remetente. SHA-256 do chunk é calculado sobre bytes originais, antes da compressão. ObjectID é SHA-256 do documento inteiro, não a concatenação dos hashes dos chunks. Dois arquivos com os mesmos bytes e nomes diferentes têm o mesmo ObjectID.

Exemplo de ordem de rede: 0x12345678 vira os bytes 12, 34, 56, 78. wire_put_u32 extrai cada byte por deslocamento; wire_get_u32 reposiciona e combina esses bytes com OR. O sufixo U de 32U ou 1U significa literal unsigned; não significa “32 bytes”.

### 25.5 Concorrência em três lugares diferentes

O runtime TCP tem 32 workers e fila de 64 conexões. Separadamente, uma transferência distribui índices de chunks entre até 32 workers, com padrão limitado a 8 e ao número de CPUs/chunks. Separadamente, o controle Unix recebe um comando local por vez. Não confunda quantidade de conexões atendidas com quantidade de chunks produzidos por uma operação.

pread/pwrite operam numa posição explícita, então dois workers não disputam o cursor de um mesmo descritor. O mutex do trabalho protege próximo índice, primeiro erro e contadores; não precisa envolver toda a transmissão TCP. No Storage, o lock do catálogo permite encontrar o documento e é solto antes do lock individual. Isso depende de os registros não serem removidos enquanto o serviço funciona.

pthread_create recebe uma função void *(*)(void *): o argumento genérico é convertido para o contexto real dentro da função. pthread_join espera a thread terminar; o ponteiro NULL devolvido pelo worker não é, por si só, o resultado de sucesso da transferência. Esse resultado está no contexto compartilhado.

### 25.6 Persistência, publicação e limites

write transfere dados ao kernel; fsync solicita durabilidade. rename publica um nome atomicamente, mas sincronizar o diretório pai também importa para persistir a mudança de nomes. pending guarda uploads incompletos e objects guarda publicados. O manifest continua versionado em v1; os payloads novos de anúncio/lookup têm operações próprias, não alteram a versão do manifest.

No download, o código atual usa link(part_path, destination), não rename que substituiria destino existente. link falha se destination já existir, inclusive numa corrida após a checagem access. Depois sincroniza o pai e remove o nome .part. Uma falha de sincronização depois de link pode deixar o destino já publicado e ainda retornar erro: atomicidade de visibilidade não é garantia de rollback de qualquer falha posterior.

No COMMIT, todos os chunks são descomprimidos em ordem para recalcular o ObjectID incrementalmente. Só depois o diretório é publicado. O anúncio no Super Peer ocorre depois da publicação local: se a rede falhar nessa etapa, podem existir bytes válidos locais sem sucesso confirmado ao usuário. Repetir a operação/reanunciar é diferente de executar uma transação distribuída 2PC, que não está implementada.

### 25.7 Como interpretar erros e testes

Não use errno depois de sucesso como se fosse um status novo. Bibliotecas pthread devolvem o código de erro diretamente; auxiliares do projeto o convertem para -1/errno. O protocolo remoto usa versão e código estáveis, porque valores numéricos de errno não são contrato portável. O canal Unix é local e devolve o errno da operação ao processo CLI.

Assert encerra um teste quando a condição falha; sua ausência de falha demonstra aquele cenário, não todos os interleavings possíveis. Texto “workers: 2” mostra configuração, não prova sobreposição real. O teste Python adicional observa eventos start/finish. Da mesma forma, não encontrar “segmentation fault” no log não equivale a provar ausência de bugs.

O comando legado leave sem identidade é mantido para compatibilidade de C1; não substitui o encerramento voluntário do Peer registrado. O NodeID de origem é declarado no header, enquanto ip_origem é observado por getpeername. Sem autenticação criptográfica, coerência de ID não constitui prova de autoria.


## 26. Catálogo detalhado — todas as 234 funções C

Esta seção usa o corpo atual da implementação, não apenas os protótipos. As listas de chamadas são referências estáticas: não são um traço de execução, não incluem automaticamente chamadas indiretas por callback e não provam que uma API sem chamador direto esteja sem uso. As expressões de retorno são mostradas literalmente para não confundir códigos de estado, ponteiros e quantidades de bytes. Em rotinas complexas, consulte também os caminhos de erro no código expansível.

- [app_config.c — 4 funções](#mod-app-config-c)
- [compression.c — 3 funções](#mod-compression-c)
- [concurrent_server.c — 5 funções](#mod-concurrent-server-c)
- [content.c — 5 funções](#mod-content-c)
- [directory.c — 4 funções](#mod-directory-c)
- [file_client.c — 17 funções](#mod-file-client-c)
- [local_control.c — 10 funções](#mod-local-control-c)
- [membership.c — 15 funções](#mod-membership-c)
- [metadata.c — 20 funções](#mod-metadata-c)
- [network.c — 9 funções](#mod-network-c)
- [node.c — 14 funções](#mod-node-c)
- [peer.c — 6 funções](#mod-peer-c)
- [peer_service.c — 10 funções](#mod-peer-service-c)
- [protocol.c — 10 funções](#mod-protocol-c)
- [remote_error.h — 2 funções](#mod-remote-error-h)
- [rpc.c — 3 funções](#mod-rpc-c)
- [storage.c — 23 funções](#mod-storage-c)
- [superpeer.c — 1 funções](#mod-superpeer-c)
- [superpeer_app.c — 21 funções](#mod-superpeer-app-c)
- [test_node_superpeer.c — 8 funções](#mod-test-node-superpeer-c)
- [teste.c — 1 funções](#mod-teste-c)
- [transfer_protocol.c — 23 funções](#mod-transfer-protocol-c)
- [wire.h — 6 funções](#mod-wire-h)
- [tests/c1/test_protocol.c — 4 funções](#mod-tests-c1-test-protocol-c)
- [tests/c2/test_metadata.c — 2 funções](#mod-tests-c2-test-metadata-c)
- [tests/c2/test_storage_atomic.c — 1 funções](#mod-tests-c2-test-storage-atomic-c)
- [tests/c2/test_transfer_negative.c — 7 funções](#mod-tests-c2-test-transfer-negative-c)

<a id="mod-app-config-c"></a>

### app_config.c

[assign](#fn-app-config-c-assign) · [trim](#fn-app-config-c-trim) · [app_config_load](#fn-app-config-c-app-config-load) · [app_identity](#fn-app-config-c-app-identity)

<a id="fn-app-config-c-assign"></a>

#### assign

Fonte: `app_config.c:15` (linha nesta revisão; pode mudar em futuras edições).

```c
static int assign(const char *key, const char *value);
```

mapeia chave para campo de configuração, verifica comprimento/IPv4/porta e rejeita chave desconhecida. Não permite resolver DNS silenciosamente.

**Parâmetros**

- `const char *key`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `const char *value`: Valor textual de configuração.

**Chamadores diretos encontrados nos fontes inventariados:** [`app_config_load` (app_config.c)](#fn-app-config-c-app-config-load).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `strtoul`: Converte texto decimal e informa onde a conversão terminou; a validação adicional restringe a faixa permitida.
- `inet_pton`: Converte endereço textual para binário; distingue 1 válido, 0 texto inválido e -1 erro da família.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de assign, para acompanhar a explicação</summary>

```c
static int assign(const char *key, const char *value)
{
    char *target = NULL;
    size_t capacity = NODE_ADDRESS_SIZE;
    if (strcmp(key, "ip") == 0 || strcmp(key, "advertise-ip") == 0) target = app_config.advertised;
    else if (strcmp(key, "bind") == 0) target = app_config.bind_ip;
    else if (strcmp(key, "superpeer-host") == 0) target = app_config.superpeer;
    else if (strcmp(key, "data-dir") == 0) { target = app_config.data_dir; capacity = sizeof(app_config.data_dir); }
    else if (strcmp(key, "port") == 0 || strcmp(key, "superpeer-port") == 0)
    {
        char *end;
        errno = 0;
        unsigned long number = strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || number == 0UL || number > UINT16_MAX) { errno = EINVAL; return -1; }
        if (strcmp(key, "port") == 0) app_config.port = (uint16_t)number;
        else app_config.superpeer_port = (uint16_t)number;
        return 0;
    }
    else { errno = EINVAL; return -1; }
    if (strlen(value) == 0U || strlen(value) >= capacity) { errno = EINVAL; return -1; }
    if (target != app_config.data_dir) { struct in_addr address; if (inet_pton(AF_INET, value, &address) != 1) { errno = EINVAL; return -1; } }
    strcpy(target, value);
    return 0;
}
```

</details>

<a id="fn-app-config-c-trim"></a>

#### trim

Fonte: `app_config.c:40` (linha nesta revisão; pode mudar em futuras edições).

```c
static char *trim(char *text);
```

elimina espaços antes/depois da linha ou valor, modificando o buffer local de parsing.

**Parâmetros**

- `char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.

**Chamadores diretos encontrados nos fontes inventariados:** [`app_config_load` (app_config.c)](#fn-app-config-c-app-config-load).

**Retorno:** as expressões presentes nesta função são `text`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de trim, para acompanhar a explicação</summary>

```c
static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}
```

</details>

<a id="fn-app-config-c-app-config-load"></a>

#### app_config_load

Fonte: `app_config.c:48` (linha nesta revisão; pode mudar em futuras edições).

```c
int app_config_load(int *argc, char **argv, int superpeer);
```

define padrões, lê arquivos chave=valor, aplica overrides e remove opções comuns de argv antes da CLI específica. Publica bind em ambiente antes de criar threads.

**Parâmetros**

- `int *argc`: Quantidade de argumentos; quando ponteiro, a função pode atualizar a contagem após retirar opções.
- `char **argv`: Vetor de argumentos terminados em NUL; as rotinas de configuração/CLI podem reorganizar seus ponteiros.
- `int superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.

**Funções do projeto usadas diretamente:** [`assign` (app_config.c)](#fn-app-config-c-assign); [`trim` (app_config.c)](#fn-app-config-c-trim).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (peer.c)](#fn-peer-c-main); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `setenv( , app_config.bind_ip, 1)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de app_config_load, para acompanhar a explicação</summary>

```c
int app_config_load(int *argc, char **argv, int superpeer)
{
    memset(&app_config, 0, sizeof(app_config));
    strcpy(app_config.advertised, "127.0.0.1");
    strcpy(app_config.bind_ip, "0.0.0.0");
    strcpy(app_config.superpeer, "127.0.0.1");
    app_config.port = superpeer ? 55101U : 55102U;
    app_config.superpeer_port = 55101U;
    for (int i = 1; i + 1 < *argc; ++i)
    {
        if (strcmp(argv[i], "--config") != 0 && strcmp(argv[i], "-f") != 0) continue;
        FILE *file = fopen(argv[i + 1], "r");
        if (file == NULL) return -1;
        char line[1024];
        int status = 0;
        while (fgets(line, sizeof(line), file) != NULL)
        {
            char *key = trim(line);
            if (*key == '#' || *key == '\0') continue;
            char *equal = strchr(key, '=');
            if (equal == NULL) { errno = EINVAL; status = -1; break; }
            *equal++ = '\0';
            if (assign(trim(key), trim(equal)) < 0) { status = -1; break; }
        }
        if (ferror(file)) status = -1;
        fclose(file);
        if (status < 0) return -1;
    }
    for (int i = 1; i < *argc; ++i)
    {
        const char *key = argv[i];
        int config = strcmp(key, "--config") == 0 || strcmp(key, "-f") == 0;
        int custom = strcmp(key, "--bind") == 0 || strcmp(key, "--advertise-ip") == 0 || strcmp(key, "--data-dir") == 0 || strcmp(key, "--superpeer-host") == 0 || strcmp(key, "--superpeer-port") == 0;
        if (!superpeer && *argc > 1 && strcmp(argv[1], "serve") == 0 && strcmp(key, "--port") == 0) custom = 1;
        if (!config && !custom) continue;
        if (i + 1 >= *argc) { errno = EINVAL; return -1; }
        if (custom && assign(key + 2, argv[i + 1]) < 0) return -1;
        for (int j = i; j + 2 < *argc; ++j) argv[j] = argv[j + 2];
        *argc -= 2;
        argv[*argc] = NULL;
        --i;
    }
    return setenv("PEER_BIND_IP", app_config.bind_ip, 1);
}
```

</details>

<a id="fn-app-config-c-app-identity"></a>

#### app_identity

Fonte: `app_config.c:94` (linha nesta revisão; pode mudar em futuras edições).

```c
int app_identity(const char *directory, NodeConfig *config, uint16_t port);
```

cria diretórios, cria UUID com exclusividade ou lê exatamente 16 bytes, sincroniza a criação e monta NodeConfig. Usa O_NOFOLLOW no arquivo UUID. NodeID é calculado por node_init, não pela leitura do arquivo.

**Parâmetros**

- `const char *directory`: Adaptador de metadados/membership quando Directory*; em const char*, caminho de diretório.
- `NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.

**Funções do projeto usadas diretamente:** [`node_generate_uuid` (node.c)](#fn-node-c-node-generate-uuid); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid).

**Chamadores diretos encontrados nos fontes inventariados:** [`initialize_service` (peer_service.c)](#fn-peer-service-c-initialize-service); [`initialize_local_identity` (superpeer_app.c)](#fn-superpeer-app-c-initialize-local-identity).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `fsync`: Solicita sincronização do descritor no armazenamento; após rename/link, sincronizar o diretório pai também importa.
- `unlink`: Remove um nome de arquivo/socket. Não é exclusão recursiva e não deve atingir arquivo que a operação não criou.
- `read`: Pode devolver menos bytes que o solicitado, zero em EOF ou -1 em erro.
- `write`: Pode gravar apenas parte dos bytes; sucesso parcial não conclui automaticamente a operação.

**Retorno:** as expressões presentes nesta função são `-1`, `node_config_init_with_uuid(config, app_config.advertised, port, uuid)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de app_identity, para acompanhar a explicação</summary>

```c
int app_identity(const char *directory, NodeConfig *config, uint16_t port)
{
    char path[600];
    uint8_t uuid[NODE_UUID_SIZE];
    if (strlen(directory) >= sizeof(path) - 12U) { errno = ENAMETOOLONG; return -1; }
    strcpy(path, directory);
    for (char *p = path + 1; *p != '\0'; ++p)
    {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    strcat(path, "/node.uuid");
    int created = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) created = 1;
    else if (errno == EEXIST) fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (created && node_generate_uuid(uuid) < 0) { close(fd); unlink(path); return -1; }
    size_t done = 0U;
    while (done < sizeof(uuid))
    {
        ssize_t count = created ? write(fd, uuid + done, sizeof(uuid) - done) : read(fd, uuid + done, sizeof(uuid) - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); if (created) unlink(path); errno = EBADMSG; return -1; }
        done += (size_t)count;
    }
    uint8_t extra;
    if ((!created && read(fd, &extra, 1U) != 0) || (created && fsync(fd) < 0)) { close(fd); errno = EBADMSG; return -1; }
    if (close(fd) < 0) return -1;
    if (created)
    {
        int directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
        if (directory_fd < 0) return -1;
        int result = fsync(directory_fd);
        close(directory_fd);
        if (result < 0) return -1;
    }
    return node_config_init_with_uuid(config, app_config.advertised, port, uuid);
}
```

</details>

<a id="mod-compression-c"></a>

### compression.c

[compression_lz4_bound](#fn-compression-c-compression-lz4-bound) · [compression_lz4_compress](#fn-compression-c-compression-lz4-compress) · [compression_lz4_decompress](#fn-compression-c-compression-lz4-decompress)

<a id="fn-compression-c-compression-lz4-bound"></a>

#### compression_lz4_bound

Fonte: `compression.c:9` (linha nesta revisão; pode mudar em futuras edições).

```c
int compression_lz4_bound(size_t input_size, size_t *bound);
```

consulta LZ4_compressBound para saber a capacidade mínima segura de saída. Rejeita tamanhos que não cabem na API int da LZ4.

**Parâmetros**

- `size_t input_size`: Quantidade de bytes legíveis no buffer de entrada.
- `size_t *bound`: Ponteiro que recebe a capacidade segura indicada pela LZ4.

**Chamadores diretos encontrados nos fontes inventariados:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EOVERFLOW`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de compression_lz4_bound, para acompanhar a explicação</summary>

```c
int compression_lz4_bound(size_t input_size, size_t *bound)
{
    int result;

    if (bound == NULL || input_size > (size_t)INT_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    result = LZ4_compressBound((int)input_size);
    if (result <= 0)
    {
        errno = EOVERFLOW;
        return -1;
    }
    *bound = (size_t)result;
    return 0;
}
```

</details>

<a id="fn-compression-c-compression-lz4-compress"></a>

#### compression_lz4_compress

Fonte: `compression.c:28` (linha nesta revisão; pode mudar em futuras edições).

```c
int compression_lz4_compress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size);
```

aloca buffer com o limite calculado, chama LZ4_compress_default e devolve buffer/tamanho real. O chamador libera output com free.

**Parâmetros**

- `const uint8_t *input`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t input_size`: Quantidade de bytes legíveis no buffer de entrada.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `size_t *output_size`: Ponteiro que recebe o tamanho produzido.

**Funções do projeto usadas diretamente:** [`compression_lz4_bound` (compression.c)](#fn-compression-c-compression-lz4-bound).

**Chamadores diretos encontrados nos fontes inventariados:** [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker); [`file_client_benchmark_lz4` (file_client.c)](#fn-file-client-c-file-client-benchmark-lz4); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `LZ4_compress_default`: Comprime um bloco na capacidade fornecida; retorno positivo é tamanho comprimido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de compression_lz4_compress, para acompanhar a explicação</summary>

```c
int compression_lz4_compress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size)
{
    uint8_t *compressed;
    size_t capacity;
    int result;

    if (input == NULL || output == NULL || output_size == NULL || input_size == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    *output_size = 0U;
    if (compression_lz4_bound(input_size, &capacity) < 0)
    {
        return -1;
    }
    compressed = malloc(capacity);
    if (compressed == NULL)
    {
        return -1;
    }
    result = LZ4_compress_default((const char *)input, (char *)compressed, (int)input_size, (int)capacity);
    if (result <= 0)
    {
        free(compressed);
        errno = EIO;
        return -1;
    }
    *output = compressed;
    *output_size = (size_t)result;
    return 0;
}
```

</details>

<a id="fn-compression-c-compression-lz4-decompress"></a>

#### compression_lz4_decompress

Fonte: `compression.c:62` (linha nesta revisão; pode mudar em futuras edições).

```c
int compression_lz4_decompress(const uint8_t *input, size_t input_size, size_t expected_size, uint8_t **output);
```

aloca a saída, chama LZ4_decompress_safe e exige que o número de bytes reconstruídos seja exatamente expected_size. Isso ajuda a rejeitar chunks malformados; o chamador libera output.

**Parâmetros**

- `const uint8_t *input`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t input_size`: Quantidade de bytes legíveis no buffer de entrada.
- `size_t expected_size`: Tamanho descomprimido exigido, usado para rejeitar conteúdo incompatível.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Chamadores diretos encontrados nos fontes inventariados:** [`download_one` (file_client.c)](#fn-file-client-c-download-one); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`verify_document` (storage.c)](#fn-storage-c-verify-document).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `LZ4_decompress_safe`: Descomprime com limites explícitos; retorno precisa coincidir com o tamanho original esperado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de compression_lz4_decompress, para acompanhar a explicação</summary>

```c
int compression_lz4_decompress(const uint8_t *input, size_t input_size, size_t expected_size, uint8_t **output)
{
    uint8_t *decompressed;
    int result;

    if (input == NULL || output == NULL || input_size == 0U || input_size > (size_t)INT_MAX || expected_size == 0U || expected_size > (size_t)INT_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    decompressed = malloc(expected_size);
    if (decompressed == NULL)
    {
        return -1;
    }
    result = LZ4_decompress_safe((const char *)input, (char *)decompressed, (int)input_size, (int)expected_size);
    if (result < 0 || (size_t)result != expected_size)
    {
        free(decompressed);
        errno = EBADMSG;
        return -1;
    }
    *output = decompressed;
    return 0;
}
```

</details>

<a id="mod-concurrent-server-c"></a>

### concurrent_server.c

[worker_run](#fn-concurrent-server-c-worker-run) · [concurrent_server_create](#fn-concurrent-server-c-concurrent-server-create) · [concurrent_server_stop](#fn-concurrent-server-c-concurrent-server-stop) · [concurrent_server_run](#fn-concurrent-server-c-concurrent-server-run) · [concurrent_server_destroy](#fn-concurrent-server-c-concurrent-server-destroy)

<a id="fn-concurrent-server-c-worker-run"></a>

#### worker_run

Fonte: `concurrent_server.c:27` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *worker_run(void *argument);
```

aguarda a condição enquanto fila está vazia; retira um descritor sob mutex, chama o handler fora do lock e fecha a conexão após o atendimento. A posição active permite interromper sockets no encerramento.

**Parâmetros**

- `void *argument`: Contexto genérico de pthread/callback, convertido para a estrutura concreta na função.

**Funções do projeto usadas diretamente:** [`network_shutdown` (network.c)](#fn-network-c-network-shutdown).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `pthread_cond_wait`: Dorme até sinalização, liberando o mutex enquanto espera e readquirindo-o ao acordar. O predicado deve ser reavaliado em laço.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de worker_run, para acompanhar a explicação</summary>

```c
static void *worker_run(void *argument)
{
    Worker *worker = argument;
    ConcurrentServer *server = worker->server;
    for (;;)
    {
        int fd;
        pthread_mutex_lock(&server->mutex);
        while (server->count == 0U && !server->stopping) pthread_cond_wait(&server->ready, &server->mutex);
        if (server->stopping) { pthread_mutex_unlock(&server->mutex); break; }
        fd = server->queue[server->head];
        server->head = (server->head + 1U) % SERVER_QUEUE;
        --server->count;
        server->active[worker->index] = fd;
        pthread_mutex_unlock(&server->mutex);
        server->handler(server->context, fd);
        pthread_mutex_lock(&server->mutex);
        server->active[worker->index] = -1;
        (void)network_shutdown(fd);
        pthread_mutex_unlock(&server->mutex);
    }
    return NULL;
}
```

</details>

<a id="fn-concurrent-server-c-concurrent-server-create"></a>

#### concurrent_server_create

Fonte: `concurrent_server.c:51` (linha nesta revisão; pode mudar em futuras edições).

```c
int concurrent_server_create(int server_fd, ConcurrentServerHandler handler, void *context, ConcurrentServer **output);
```

valida argumentos e inicializa mutex, condição e 32 workers. Se uma criação falhar, acorda/aguarda os já criados e devolve erro sem assumir ownership do listener.

**Parâmetros**

- `int server_fd`: Descritor do listener; aceitação produz outro descritor.
- `ConcurrentServerHandler handler`: Callback chamado pelo runtime para atender a conexão.
- `void *context`: Contexto compartilhado entregue ao callback; seu tipo real é definido pelo módulo.
- `ConcurrentServer **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `pthread_cond_broadcast`: Acorda os workers para que reavaliem fila e estado de parada; não garante que já terminaram.
- `pthread_create`: Inicia thread com callback e contexto; erro vem no retorno pthread, não necessariamente em errno.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de concurrent_server_create, para acompanhar a explicação</summary>

```c
int concurrent_server_create(int server_fd, ConcurrentServerHandler handler, void *context, ConcurrentServer **output)
{
    ConcurrentServer *server;
    int error;
    if (server_fd < 0 || handler == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    server = calloc(1U, sizeof(*server));
    if (server == NULL) return -1;
    server->server_fd = server_fd;
    server->handler = handler;
    server->context = context;
    for (size_t i = 0U; i < SERVER_WORKERS; ++i) server->active[i] = -1;
    error = pthread_mutex_init(&server->mutex, NULL);
    if (error != 0) { free(server); errno = error; return -1; }
    error = pthread_cond_init(&server->ready, NULL);
    if (error != 0) { pthread_mutex_destroy(&server->mutex); free(server); errno = error; return -1; }
    for (size_t i = 0U; i < SERVER_WORKERS; ++i)
    {
        server->workers[i].server = server;
        server->workers[i].index = i;
        error = pthread_create(&server->threads[i], NULL, worker_run, &server->workers[i]);
        if (error != 0)
        {
            pthread_mutex_lock(&server->mutex);
            server->stopping = 1;
            pthread_cond_broadcast(&server->ready);
            pthread_mutex_unlock(&server->mutex);
            for (size_t j = 0U; j < server->started; ++j) pthread_join(server->threads[j], NULL);
            pthread_cond_destroy(&server->ready);
            pthread_mutex_destroy(&server->mutex);
            free(server);
            errno = error;
            return -1;
        }
        ++server->started;
    }
    *output = server;
    return 0;
}
```

</details>

<a id="fn-concurrent-server-c-concurrent-server-stop"></a>

#### concurrent_server_stop

Fonte: `concurrent_server.c:91` (linha nesta revisão; pode mudar em futuras edições).

```c
void concurrent_server_stop(ConcurrentServer *server);
```

marca stopping, faz shutdown no listener e nas conexões ativas, fecha pendentes e acorda workers. É idempotente.

**Parâmetros**

- `ConcurrentServer *server`: Estado do runtime concorrente e seus sockets/threads.

**Chamadores diretos encontrados nos fontes inventariados:** [`concurrent_server_run` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-run); [`concurrent_server_destroy` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-destroy); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `pthread_cond_broadcast`: Acorda os workers para que reavaliem fila e estado de parada; não garante que já terminaram.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `shutdown`: Interrompe direções de comunicação do socket; o descritor ainda precisa de close.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de concurrent_server_stop, para acompanhar a explicação</summary>

```c
void concurrent_server_stop(ConcurrentServer *server)
{
    if (server == NULL) return;
    pthread_mutex_lock(&server->mutex);
    server->stopping = 1;
    (void)shutdown(server->server_fd, SHUT_RDWR);
    for (size_t i = 0U; i < SERVER_WORKERS; ++i) if (server->active[i] >= 0) (void)shutdown(server->active[i], SHUT_RDWR);
    while (server->count != 0U)
    {
        close(server->queue[server->head]);
        server->head = (server->head + 1U) % SERVER_QUEUE;
        --server->count;
    }
    pthread_cond_broadcast(&server->ready);
    pthread_mutex_unlock(&server->mutex);
}
```

</details>

<a id="fn-concurrent-server-c-concurrent-server-run"></a>

#### concurrent_server_run

Fonte: `concurrent_server.c:108` (linha nesta revisão; pode mudar em futuras edições).

```c
int concurrent_server_run(ConcurrentServer *server);
```

executa accept e insere na fila de até 64 sockets. Se cheia, fecha a conexão excedente. Ao sair solicita parada.

**Parâmetros**

- `ConcurrentServer *server`: Estado do runtime concorrente e seus sockets/threads.

**Funções do projeto usadas diretamente:** [`concurrent_server_stop` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-stop); [`network_accept_client` (network.c)](#fn-network-c-network-accept-client).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`accept_clients` (superpeer_app.c)](#fn-superpeer-app-c-accept-clients).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de concurrent_server_run, para acompanhar a explicação</summary>

```c
int concurrent_server_run(ConcurrentServer *server)
{
    if (server == NULL) { errno = EINVAL; return -1; }
    for (;;)
    {
        int fd = network_accept_client(server->server_fd);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pthread_mutex_lock(&server->mutex);
        if (server->stopping || server->count == SERVER_QUEUE) close(fd);
        else
        {
            server->queue[(server->head + server->count) % SERVER_QUEUE] = fd;
            ++server->count;
            pthread_cond_signal(&server->ready);
        }
        int stopping = server->stopping;
        pthread_mutex_unlock(&server->mutex);
        if (stopping) break;
    }
    concurrent_server_stop(server);
    return 0;
}
```

</details>

<a id="fn-concurrent-server-c-concurrent-server-destroy"></a>

#### concurrent_server_destroy

Fonte: `concurrent_server.c:131` (linha nesta revisão; pode mudar em futuras edições).

```c
void concurrent_server_destroy(ConcurrentServer *server);
```

solicita parada, aguarda todos os workers, fecha listener, destrói sincronização e libera memória. O serviço deve aguardar o laço accept antes de destruir.

**Parâmetros**

- `ConcurrentServer *server`: Estado do runtime concorrente e seus sockets/threads.

**Funções do projeto usadas diretamente:** [`concurrent_server_stop` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-stop).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `pthread_join`: Aguarda a thread, garantindo que o contexto dela não seja liberado enquanto ainda estiver em uso.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de concurrent_server_destroy, para acompanhar a explicação</summary>

```c
void concurrent_server_destroy(ConcurrentServer *server)
{
    if (server == NULL) return;
    concurrent_server_stop(server);
    for (size_t i = 0U; i < server->started; ++i) pthread_join(server->threads[i], NULL);
    close(server->server_fd);
    pthread_cond_destroy(&server->ready);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}
```

</details>

<a id="mod-content-c"></a>

### content.c

[content_sha256](#fn-content-c-content-sha256) · [content_validate_pdf](#fn-content-c-content-validate-pdf) · [content_basename](#fn-content-c-content-basename) · [content_pdf_name](#fn-content-c-content-pdf-name) · [content_sync_parent](#fn-content-c-content-sync-parent)

<a id="fn-content-c-content-sha256"></a>

#### content_sha256

Fonte: `content.c:13` (linha nesta revisão; pode mudar em futuras edições).

```c
int content_sha256(const uint8_t *data, size_t size, uint8_t digest[OBJECT_ID_SIZE]);
```

calcula SHA-256 de um buffer por libcrypto, usado nos chunks. Não calcula o ObjectID de arquivo inteiro; isso cabe a object_id_file.

**Parâmetros**

- `const uint8_t *data`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.
- `uint8_t digest[OBJECT_ID_SIZE]`: Buffer para os 32 bytes do SHA-256 calculado.

**Chamadores diretos encontrados nos fontes inventariados:** [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker); [`download_one` (file_client.c)](#fn-file-client-c-download-one); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`verify_document` (storage.c)](#fn-storage-c-verify-document); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `SHA256`: Usa OpenSSL para calcular o digest do buffer; não é implementação manual do algoritmo.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de content_sha256, para acompanhar a explicação</summary>

```c
int content_sha256(const uint8_t *data, size_t size, uint8_t digest[OBJECT_ID_SIZE])
{
    if (digest == NULL || (data == NULL && size != 0U))
    {
        errno = EINVAL;
        return -1;
    }
    if (SHA256(data, size, digest) == NULL)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-content-c-content-validate-pdf"></a>

#### content_validate_pdf

Fonte: `content.c:28` (linha nesta revisão; pode mudar em futuras edições).

```c
int content_validate_pdf(const char *path);
```

Exige extensão .pdf sem distinção de maiúsculas e tenta abrir o arquivo. Não exige assinatura %PDF e não faz parse semântico. Isso admite os arquivos sintéticos do professor.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Funções do projeto usadas diretamente:** [`content_pdf_name` (content.c)](#fn-content-c-content-pdf-name).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`file_client_benchmark_lz4` (file_client.c)](#fn-file-client-c-file-client-benchmark-lz4).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de content_validate_pdf, para acompanhar a explicação</summary>

```c
int content_validate_pdf(const char *path)
{
    FILE *file;

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (!content_pdf_name(path)) { errno = EINVAL; return -1; }
    file = fopen(path, "rb");
    if (file == NULL)
    {
        return -1;
    }
    if (fclose(file) != 0)
    {
        return -1;
    }
    /* C2 valida o tipo pela extensão; fixtures sintéticas também são aceitas. */
    return 0;
}
```

</details>

<a id="fn-content-c-content-basename"></a>

#### content_basename

Fonte: `content.c:51` (linha nesta revisão; pode mudar em futuras edições).

```c
int content_basename(const char *path, char output[METADATA_NAME_SIZE]);
```

extrai o último componente após / e valida se cabe em METADATA_NAME_SIZE. Esse nome é metadado; caminhos físicos internos usam ObjectID, não o nome recebido.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.
- `char output[METADATA_NAME_SIZE]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de content_basename, para acompanhar a explicação</summary>

```c
int content_basename(const char *path, char output[METADATA_NAME_SIZE])
{
    const char *name;
    const char *slash;
    size_t length;

    if (path == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(path, '/');
    name = slash == NULL ? path : slash + 1;
    length = strlen(name);
    if (length == 0U || length >= METADATA_NAME_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    memcpy(output, name, length + 1U);
    return 0;
}
```

</details>

<a id="fn-content-c-content-pdf-name"></a>

#### content_pdf_name

Fonte: `content.c:74` (linha nesta revisão; pode mudar em futuras edições).

```c
int content_pdf_name(const char *path);
```

verifica apenas extensão .pdf case-insensitive, sem acessar disco.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Chamadores diretos encontrados nos fontes inventariados:** [`content_validate_pdf` (content.c)](#fn-content-c-content-validate-pdf); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin).

**Retorno:** as expressões presentes nesta função são `0`, `length >= 4U && path[length - 4U] == && tolower((unsigned char)path[length - 3U]) == && tolower((unsigned char)path[length - 2U]) == && tolower((unsigned char)path[length - 1U]) ==`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de content_pdf_name, para acompanhar a explicação</summary>

```c
int content_pdf_name(const char *path)
{
    if (path == NULL) return 0;
    size_t length = strlen(path);
    return length >= 4U && path[length - 4U] == '.' && tolower((unsigned char)path[length - 3U]) == 'p' && tolower((unsigned char)path[length - 2U]) == 'd' && tolower((unsigned char)path[length - 1U]) == 'f';
}
```

</details>

<a id="fn-content-c-content-sync-parent"></a>

#### content_sync_parent

Fonte: `content.c:81` (linha nesta revisão; pode mudar em futuras edições).

```c
int content_sync_parent(const char *path);
```

abre e sincroniza diretório pai após publicação de entrada de arquivo.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `fsync`: Solicita sincronização do descritor no armazenamento; após rename/link, sincronizar o diretório pai também importa.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de content_sync_parent, para acompanhar a explicação</summary>

```c
int content_sync_parent(const char *path)
{
    char parent[4096];
    if (path == NULL || strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == NULL) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int status = fsync(fd);
    int error = errno;
    close(fd);
    errno = error;
    return status;
}
```

</details>

<a id="mod-directory-c"></a>

### directory.c

[directory_create](#fn-directory-c-directory-create) · [directory_destroy](#fn-directory-c-directory-destroy) · [directory_announce](#fn-directory-c-directory-announce) · [directory_lookup](#fn-directory-c-directory-lookup)

<a id="fn-directory-c-directory-create"></a>

#### directory_create

Fonte: `directory.c:9` (linha nesta revisão; pode mudar em futuras edições).

```c
int directory_create(MetadataStore *metadata, SuperPeer *superpeer, Directory **output);
```

Aloca adaptador leve contendo ponteiros emprestados ao índice e à tabela de membros; não mantém outra lista de nomes.

**Parâmetros**

- `MetadataStore *metadata`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `Directory **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de directory_create, para acompanhar a explicação</summary>

```c
int directory_create(MetadataStore *metadata, SuperPeer *superpeer, Directory **output)
{
    if (metadata == NULL || superpeer == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = calloc(1U, sizeof(**output));
    if (*output == NULL) return -1;
    (*output)->metadata = metadata;
    (*output)->superpeer = superpeer;
    return 0;
}
```

</details>

<a id="fn-directory-c-directory-destroy"></a>

#### directory_destroy

Fonte: `directory.c:19` (linha nesta revisão; pode mudar em futuras edições).

```c
void directory_destroy(Directory *directory);
```

Libera somente o adaptador; metadados e membership pertencem ao serviço.

**Parâmetros**

- `Directory *directory`: Adaptador de metadados/membership quando Directory*; em const char*, caminho de diretório.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de directory_destroy, para acompanhar a explicação</summary>

```c
void directory_destroy(Directory *directory) { free(directory); }
```

</details>

<a id="fn-directory-c-directory-announce"></a>

#### directory_announce

Fonte: `directory.c:22` (linha nesta revisão; pode mudar em futuras edições).

```c
int directory_announce(Directory *directory, const TransferDocument *document, const MetadataChunk *chunks, const NodeID *owner);
```

Valida compressão/extensão e adapta TransferDocument para MetadataDocument; chama metadata_announce com descritores e dono. Não cadastra chunks um a um.

**Parâmetros**

- `Directory *directory`: Adaptador de metadados/membership quando Directory*; em const char*, caminho de diretório.
- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `const MetadataChunk *chunks`: Vetor de descritores/localizações; quantidade vem do documento associado.
- `const NodeID *owner`: NodeID do nó proprietário/anunciante, não um PID.

**Funções do projeto usadas diretamente:** [`metadata_announce` (metadata.c)](#fn-metadata-c-metadata-announce).

**Chamadores diretos encontrados nos fontes inventariados:** [`register_announcement` (superpeer_app.c)](#fn-superpeer-app-c-register-announcement).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `metadata_announce(directory->metadata, &metadata, chunks, owner)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de directory_announce, para acompanhar a explicação</summary>

```c
int directory_announce(Directory *directory, const TransferDocument *document, const MetadataChunk *chunks, const NodeID *owner)
{
    if (directory == NULL || document == NULL || owner == NULL || document->compression != COMPRESSION_LZ4) { errno = EINVAL; return -1; }
    size_t length = strlen(document->name);
    if (length < 4U || strcasecmp(document->name + length - 4U, ".pdf") != 0) { errno = EINVAL; return -1; }
    MetadataDocument metadata = {.id = document->id, .file_size = document->file_size, .chunk_count = document->chunk_count, .version = 1U, .owner = *owner, .compression = document->compression};
    memcpy(metadata.name, document->name, sizeof(metadata.name));
    return metadata_announce(directory->metadata, &metadata, chunks, owner);
}
```

</details>

<a id="fn-directory-c-directory-lookup"></a>

#### directory_lookup

Fonte: `directory.c:32` (linha nesta revisão; pode mudar em futuras edições).

```c
int directory_lookup(Directory *directory, TransferSelectorType type, const ObjectID *id, const char *name, TransferLookupResult *result);
```

Resolve nome na hash table, copia documento/descritores/localizações e traduz NodeIDs para endpoints com membership. ENOTUNIQ indica ambiguidade; ENODATA indica chunk sem localização ativa.

**Parâmetros**

- `Directory *directory`: Adaptador de metadados/membership quando Directory*; em const char*, caminho de diretório.
- `TransferSelectorType type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `const char *name`: Nome textual usado como chave, metadado ou variável de ambiente, conforme a finalidade descrita.
- `TransferLookupResult *result`: Estrutura de resultado de lookup, com documento e vetor de localizações; liberar por transfer_lookup_result_free.

**Passo a passo**

1. Resolve seletor por nome ou usa o ObjectID fornecido.
2. Obtém cópia dos metadados do documento e reserva vetor de informações por chunk.
3. Para cada chunk, copia o descritor e obtém NodeIDs que anunciaram disponibilidade.
4. Consulta membership para transformar cada NodeID em IP/porta; não transfere conteúdo aqui.
5. Monta TransferLookupResult ou libera parcialmente a estrutura ao falhar. Nome ambíguo e ausência de localização têm erros distintos.

**Funções do projeto usadas diretamente:** [`superpeer_find_member` (membership.c)](#fn-membership-c-superpeer-find-member); [`metadata_find_document` (metadata.c)](#fn-metadata-c-metadata-find-document); [`metadata_chunk_peers` (metadata.c)](#fn-metadata-c-metadata-chunk-peers); [`metadata_find_name` (metadata.c)](#fn-metadata-c-metadata-find-name); [`metadata_chunk_descriptor` (metadata.c)](#fn-metadata-c-metadata-chunk-descriptor); [`transfer_lookup_result_free` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-lookup-result-free).

**Chamadores diretos encontrados nos fontes inventariados:** [`answer_lookup` (superpeer_app.c)](#fn-superpeer-app-c-answer-lookup).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENODATA`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de directory_lookup, para acompanhar a explicação</summary>

```c
int directory_lookup(Directory *directory, TransferSelectorType type, const ObjectID *id, const char *name, TransferLookupResult *result)
{
    ObjectID selected;
    MetadataDocument metadata_document;
    uint64_t chunk_index;

    if (directory == NULL || result == NULL || (type == TRANSFER_SELECTOR_OBJECT_ID && id == NULL) || (type == TRANSFER_SELECTOR_NAME && name == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        selected = *id;
    }
    else if (metadata_find_name(directory->metadata, name, &selected) < 0)
    {
        return -1;
    }
    if (metadata_find_document(directory->metadata, &selected, &metadata_document) < 0 || metadata_document.chunk_count > SIZE_MAX / sizeof(*result->chunks))
    {
        return -1;
    }
    result->document.id = metadata_document.id;
    strcpy(result->document.name, metadata_document.name);
    result->document.file_size = metadata_document.file_size;
    result->document.chunk_count = metadata_document.chunk_count;
    result->document.compression = COMPRESSION_LZ4;
    if (metadata_document.chunk_count != 0U)
    {
        result->chunks = calloc((size_t)metadata_document.chunk_count, sizeof(*result->chunks));
        if (result->chunks == NULL)
        {
            return -1;
        }
    }
    for (chunk_index = 0U; chunk_index < metadata_document.chunk_count; ++chunk_index)
    {
        if (metadata_chunk_descriptor(directory->metadata, &selected, chunk_index, &result->chunks[chunk_index].descriptor) < 0) { transfer_lookup_result_free(result); return -1; }
        NodeID *owners = NULL;
        size_t owner_count = 0U;
        size_t owner_index;
        size_t valid_count = 0U;
        TransferEndpoint *endpoints = NULL;

        if (metadata_chunk_peers(directory->metadata, &selected, chunk_index, &owners, &owner_count) < 0)
        {
            transfer_lookup_result_free(result);
            return -1;
        }
        if (owner_count != 0U)
        {
            endpoints = calloc(owner_count, sizeof(*endpoints));
            if (endpoints == NULL)
            {
                free(owners);
                transfer_lookup_result_free(result);
                return -1;
            }
        }
        for (owner_index = 0U; owner_index < owner_count; ++owner_index)
        {
            SuperPeerMember member;

            if (superpeer_find_member(directory->superpeer, &owners[owner_index], &member) == 0)
            {
                TransferEndpoint *endpoint = &endpoints[valid_count++];

                endpoint->node_id = owners[owner_index];
                strcpy(endpoint->ip, member.node.config.ip);
                endpoint->port = member.node.config.port;
            }
        }
        free(owners);
        result->chunks[chunk_index].peers = endpoints;
        result->chunks[chunk_index].peer_count = valid_count;
        if (valid_count == 0U)
        {
            transfer_lookup_result_free(result);
            errno = ENODATA;
            return -1;
        }
    }
    return 0;
}
```

</details>

<a id="mod-file-client-c"></a>

### file_client.c

[transfer_worker_count](#fn-file-client-c-transfer-worker-count) · [request_expect](#fn-file-client-c-request-expect) · [discover_peer](#fn-file-client-c-discover-peer) · [pread_all](#fn-file-client-c-pread-all) · [pwrite_all](#fn-file-client-c-pwrite-all) · [upload_fail](#fn-file-client-c-upload-fail) · [upload_worker](#fn-file-client-c-upload-worker) · [execute_workers](#fn-file-client-c-execute-workers) · [execute_upload_workers](#fn-file-client-c-execute-upload-workers) · [file_client_upload](#fn-file-client-c-file-client-upload) · [download_fail](#fn-file-client-c-download-fail) · [download_one](#fn-file-client-c-download-one) · [download_worker](#fn-file-client-c-download-worker) · [execute_download_workers](#fn-file-client-c-execute-download-workers) · [lookup_document](#fn-file-client-c-lookup-document) · [file_client_download](#fn-file-client-c-file-client-download) · [file_client_benchmark_lz4](#fn-file-client-c-file-client-benchmark-lz4)

<a id="fn-file-client-c-transfer-worker-count"></a>

#### transfer_worker_count

Fonte: `file_client.c:57` (linha nesta revisão; pode mudar em futuras edições).

```c
static size_t transfer_worker_count(uint64_t chunk_count);
```

lê número de CPUs e eventual variável PEER_TRANSFER_THREADS; limita a quantidade ao número de chunks e impede zero workers.

**Parâmetros**

- `uint64_t chunk_count`: Número total de chunks da operação.

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `strtoul`: Converte texto decimal e informa onde a conversão terminou; a validação adicional restringe a faixa permitida.

**Retorno:** as expressões presentes nesta função são `wanted == 0UL ? 1U : (size_t)wanted`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de transfer_worker_count, para acompanhar a explicação</summary>

```c
static size_t transfer_worker_count(uint64_t chunk_count)
{
    const char *configured = getenv("PEER_TRANSFER_THREADS");
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long wanted = cpu_count > 0L ? (unsigned long)cpu_count : 1UL;

    if (wanted > DEFAULT_MAX_WORKERS)
    {
        wanted = DEFAULT_MAX_WORKERS;
    }
    if (configured != NULL && configured[0] != '\0')
    {
        char *end = NULL;
        unsigned long value;

        errno = 0;
        value = strtoul(configured, &end, 10);
        if (errno == 0 && end != configured && *end == '\0' && value >= 1UL && value <= MAX_CONFIGURED_WORKERS)
        {
            wanted = value;
        }
    }
    if ((uint64_t)wanted > chunk_count)
    {
        wanted = (unsigned long)chunk_count;
    }
    return wanted == 0UL ? 1U : (size_t)wanted;
}
```

</details>

<a id="fn-file-client-c-request-expect"></a>

#### request_expect

Fonte: `file_client.c:86` (linha nesta revisão; pode mudar em futuras edições).

```c
static int request_expect(const FileSession *session, const NodeID *destination, const char *host, uint16_t port, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message_Type expected, Message *response);
```

Chama rpc_call com sessão e destino esperados, confere o tipo da resposta e traduz ERROR wire para errno. Libera resposta em erro.

**Parâmetros**

- `const FileSession *session`: Identidade do Peer executor, FILE de progresso e cwd de origem da CLI.
- `const NodeID *destination`: NodeID remoto esperado; nulo é permitido para descoberta inicial.
- `const char *host`: Endereço IPv4 textual do endpoint remoto.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `Message_Type type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `uint32_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `Message_Type expected`: Tipo de resposta esperado pela operação.
- `Message *response`: Message de saída; o payload recebido precisa de message_free quando a chamada termina com sucesso.

**Funções do projeto usadas diretamente:** [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`remote_error_decode` (remote_error.h)](#fn-remote-error-h-remote-error-decode); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call).

**Chamadores diretos encontrados nos fontes inventariados:** [`discover_peer` (file_client.c)](#fn-file-client-c-discover-peer); [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker); [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`download_one` (file_client.c)](#fn-file-client-c-download-one); [`lookup_document` (file_client.c)](#fn-file-client-c-lookup-document).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de request_expect, para acompanhar a explicação</summary>

```c
static int request_expect(const FileSession *session, const NodeID *destination, const char *host, uint16_t port, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message_Type expected, Message *response)
{
    if (rpc_call(host, port, &session->source, destination, type, payload, payload_size, response) < 0)
    {
        return -1;
    }
    if (response->header.message_type != (uint8_t)expected)
    {
        int error = response->header.message_type == M_ERROR ? remote_error_decode(response->payload, response->header.payload_size) : EBADMSG;
        message_free(response);
        errno = error;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-file-client-c-discover-peer"></a>

#### discover_peer

Fonte: `file_client.c:103` (linha nesta revisão; pode mudar em futuras edições).

```c
static int discover_peer(const FileSession *session, const char *host, uint16_t port, NodeID *id);
```

envia PING com sessão do serviço, exige PONG e NodeID não nulo; usa esse destino nas operações seguintes.

**Parâmetros**

- `const FileSession *session`: Identidade do Peer executor, FILE de progresso e cwd de origem da CLI.
- `const char *host`: Endereço IPv4 textual do endpoint remoto.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `NodeID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Funções do projeto usadas diretamente:** [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`message_free` (protocol.c)](#fn-protocol-c-message-free).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`lookup_document` (file_client.c)](#fn-file-client-c-lookup-document).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de discover_peer, para acompanhar a explicação</summary>

```c
static int discover_peer(const FileSession *session, const char *host, uint16_t port, NodeID *id)
{
    Message reply;
    const uint8_t zero[NODE_ID_SIZE] = {0};
    if (request_expect(session, NULL, host, port, M_PING, (const uint8_t *)"PING", 4U, M_PONG, &reply) < 0) return -1;
    memcpy(id->bytes, reply.header.source_node, NODE_ID_SIZE);
    message_free(&reply);
    if (memcmp(id->bytes, zero, NODE_ID_SIZE) == 0) { errno = EBADMSG; return -1; }
    return 0;
}
```

</details>

<a id="fn-file-client-c-pread-all"></a>

#### pread_all

Fonte: `file_client.c:114` (linha nesta revisão; pode mudar em futuras edições).

```c
static int pread_all(int fd, uint8_t *buffer, size_t size, uint64_t offset);
```

lê um chunk inteiro em posição explícita do arquivo sem compartilhar o offset do descritor entre threads. Repete leituras parciais e EINTR.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `uint8_t *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.
- `uint64_t offset`: Posição em bytes no arquivo/buffer, não número do chunk.

**Chamadores diretos encontrados nos fontes inventariados:** [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker); [`file_client_benchmark_lz4` (file_client.c)](#fn-file-client-c-file-client-benchmark-lz4).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pread`: Lê de um offset explícito sem deslocar o cursor compartilhado do descritor, adequado aos workers.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EOVERFLOW`, `EIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de pread_all, para acompanhar a explicação</summary>

```c
static int pread_all(int fd, uint8_t *buffer, size_t size, uint64_t offset)
{
    size_t done = 0U;

    if (offset > (uint64_t)INT64_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    while (done < size)
    {
        ssize_t count = pread(fd, buffer + done, size - done, (off_t)(offset + done));

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            if (count == 0)
            {
                errno = EIO;
            }
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}
```

</details>

<a id="fn-file-client-c-pwrite-all"></a>

#### pwrite_all

Fonte: `file_client.c:144` (linha nesta revisão; pode mudar em futuras edições).

```c
static int pwrite_all(int fd, const uint8_t *buffer, size_t size, uint64_t offset);
```

grava um chunk inteiro no offset correto do destino, também seguro contra interferência entre offsets de workers distintos. Repete escritas parciais e EINTR.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `const uint8_t *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.
- `uint64_t offset`: Posição em bytes no arquivo/buffer, não número do chunk.

**Chamadores diretos encontrados nos fontes inventariados:** [`download_one` (file_client.c)](#fn-file-client-c-download-one).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pwrite`: Grava em offset explícito; escritas parciais ainda precisam de laço.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EOVERFLOW`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de pwrite_all, para acompanhar a explicação</summary>

```c
static int pwrite_all(int fd, const uint8_t *buffer, size_t size, uint64_t offset)
{
    size_t done = 0U;

    if (offset > (uint64_t)INT64_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    while (done < size)
    {
        ssize_t count = pwrite(fd, buffer + done, size - done, (off_t)(offset + done));

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}
```

</details>

<a id="fn-file-client-c-upload-fail"></a>

#### upload_fail

Fonte: `file_client.c:170` (linha nesta revisão; pode mudar em futuras edições).

```c
static void upload_fail(void *context, int error);
```

Converte o contexto genérico para UploadWork. Sob o mutex do trabalho, armazena somente o primeiro erro; as próximas chamadas não substituem a causa inicial. Os workers consultam esse campo antes de reservar novos índices.

**Parâmetros**

- `void *context`: Contexto compartilhado entregue ao callback; seu tipo real é definido pelo módulo.
- `int error`: Código de erro a registrar ou codificar.

**Chamadores diretos encontrados nos fontes inventariados:** [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de upload_fail, para acompanhar a explicação</summary>

```c
static void upload_fail(void *context, int error)
{
    UploadWork *work = context;
    (void)pthread_mutex_lock(&work->mutex);
    if (!work->failed)
    {
        work->failed = 1;
        work->saved_errno = error == 0 ? EIO : error;
    }
    (void)pthread_mutex_unlock(&work->mutex);
}
```

</details>

<a id="fn-file-client-c-upload-worker"></a>

#### upload_worker

Fonte: `file_client.c:182` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *upload_worker(void *argument);
```

pega próximo índice, lê bytes originais por pread_all, calcula SHA-256 do chunk, comprime com LZ4, codifica STORE/CHUNK e exige ACK do Peer. Atualiza total comprimido e hashes de saída; libera buffers temporários.

**Parâmetros**

- `void *argument`: Contexto genérico de pthread/callback, convertido para a estrutura concreta na função.

**Funções do projeto usadas diretamente:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`pread_all` (file_client.c)](#fn-file-client-c-pread-all); [`upload_fail` (file_client.c)](#fn-file-client-c-upload-fail); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`transfer_encode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-chunk).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de upload_worker, para acompanhar a explicação</summary>

```c
static void *upload_worker(void *argument)
{
    UploadWork *work = argument;

    for (;;)
    {
        uint64_t index;
        uint64_t offset;
        uint64_t remaining;
        size_t raw_size;
        uint8_t *raw = NULL;
        uint8_t *compressed = NULL;
        size_t compressed_size = 0U;
        uint8_t *payload = NULL;
        uint32_t payload_size = 0U;
        TransferChunk chunk;
        Message response;

        (void)pthread_mutex_lock(&work->mutex);
        if (work->failed || work->next_index >= work->document.chunk_count)
        {
            (void)pthread_mutex_unlock(&work->mutex);
            break;
        }
        index = work->next_index++;
        work->state = TRANSFER_STARTED;
        (void)pthread_mutex_unlock(&work->mutex);
        (void)pthread_mutex_lock(&work->mutex);
        work->state = TRANSFER_TRANSFERRING;
        (void)pthread_mutex_unlock(&work->mutex);
        fprintf(work->session->output, "Worker start chunk %" PRIu64 "\n", index);
        offset = index * METADATA_CHUNK_SIZE;
        remaining = work->document.file_size - offset;
        raw_size = remaining > METADATA_CHUNK_SIZE ? (size_t)METADATA_CHUNK_SIZE : (size_t)remaining;
        raw = malloc(raw_size);
        if (raw == NULL || pread_all(work->input_fd, raw, raw_size, offset) < 0)
        {
            upload_fail(work, errno);
            free(raw);
            break;
        }
        memset(&chunk, 0, sizeof(chunk));
        chunk.id = work->document.id;
        chunk.index = index;
        chunk.offset = offset;
        chunk.raw_size = (uint32_t)raw_size;
        if (content_sha256(raw, raw_size, chunk.hash) < 0 || compression_lz4_compress(raw, raw_size, &compressed, &compressed_size) < 0 || compressed_size > UINT32_MAX)
        {
            upload_fail(work, errno);
            free(raw);
            free(compressed);
            break;
        }
        chunk.compressed_size = (uint32_t)compressed_size;
        chunk.data = compressed;
        memcpy(work->hashes + (size_t)index * OBJECT_ID_SIZE, chunk.hash, OBJECT_ID_SIZE);
        if (transfer_encode_chunk(&chunk, TRANSFER_STORE_CHUNK, &payload, &payload_size) < 0 || request_expect(work->session, &work->destination, work->host, work->port, M_STORE, payload, payload_size, M_ACK, &response) < 0)
        {
            upload_fail(work, errno);
            free(payload);
            free(compressed);
            free(raw);
            break;
        }
        message_free(&response);
        (void)pthread_mutex_lock(&work->mutex);
        work->compressed_total += compressed_size;
        fprintf(work->session->output, "Worker finish chunk %" PRIu64 "\n", index);
        (void)pthread_mutex_unlock(&work->mutex);
        free(payload);
        free(compressed);
        free(raw);
    }
    return NULL;
}
```

</details>

<a id="fn-file-client-c-execute-workers"></a>

#### execute_workers

Fonte: `file_client.c:259` (linha nesta revisão; pode mudar em futuras edições).

```c
static int execute_workers(void *work, size_t count, void *(*worker)(void *), void (*fail)(void *, int));
```

gerencia criação/join dos workers de uma operação; o callback de falha interrompe distribuição de novas tarefas. Wrappers execute_upload_workers/execute_download_workers propagam o primeiro erro preservado.

**Parâmetros**

- `void *work`: Estado da transferência, com índices, erro, contadores e mutex.
- `size_t count`: Quantidade de elementos ou ponteiro para devolvê-la; não necessariamente bytes.
- `void *(*worker)(void *)`: Callback executado por cada thread.
- `void (*fail)(void *, int)`: Callback que registra falha no contexto da transferência.

**Chamadores diretos encontrados nos fontes inventariados:** [`execute_upload_workers` (file_client.c)](#fn-file-client-c-execute-upload-workers); [`execute_download_workers` (file_client.c)](#fn-file-client-c-execute-download-workers).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_create`: Inicia thread com callback e contexto; erro vem no retorno pthread, não necessariamente em errno.
- `pthread_join`: Aguarda a thread, garantindo que o contexto dela não seja liberado enquanto ainda estiver em uso.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de execute_workers, para acompanhar a explicação</summary>

```c
static int execute_workers(void *work, size_t count, void *(*worker)(void *), void (*fail)(void *, int))
{
    pthread_t threads[MAX_CONFIGURED_WORKERS];
    size_t started = 0U;
    int join_error = 0;
    if (count == 0U || count > MAX_CONFIGURED_WORKERS) { errno = EINVAL; return -1; }
    for (size_t i = 0U; i < count; ++i)
    {
        int error = pthread_create(&threads[i], NULL, worker, work);
        if (error != 0) { fail(work, error); break; }
        ++started;
    }
    for (size_t i = 0U; i < started; ++i)
    {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) { fail(work, error); join_error = error; }
    }
    if (join_error != 0) { errno = join_error; return -1; }
    return 0;
}
```

</details>

<a id="fn-file-client-c-execute-upload-workers"></a>

#### execute_upload_workers

Fonte: `file_client.c:280` (linha nesta revisão; pode mudar em futuras edições).

```c
static int execute_upload_workers(UploadWork *work, size_t worker_count);
```

Passa UploadWork, a quantidade de threads, upload_worker e upload_fail ao executor compartilhado. Depois do término consulta o erro guardado no contexto, repõe errno e devolve falha se algum worker não completou.

**Parâmetros**

- `UploadWork *work`: Estado da transferência, com índices, erro, contadores e mutex.
- `size_t worker_count`: Quantidade de workers a iniciar para a transferência.

**Funções do projeto usadas diretamente:** [`execute_workers` (file_client.c)](#fn-file-client-c-execute-workers).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de execute_upload_workers, para acompanhar a explicação</summary>

```c
static int execute_upload_workers(UploadWork *work, size_t worker_count)
{
    if (execute_workers(work, worker_count, upload_worker, upload_fail) < 0) return -1;
    if (work->failed) { errno = work->saved_errno; return -1; }
    return 0;
}
```

</details>

<a id="fn-file-client-c-file-client-upload"></a>

#### file_client_upload

Fonte: `file_client.c:287` (linha nesta revisão; pode mudar em futuras edições).

```c
int file_client_upload(const FileSession *session, const char *path, const char *peer_host, uint16_t peer_port);
```

Recebe FileSession do Peer ativo e FILE de progresso; valida extensão, calcula ObjectID incremental, descobre destino por PING e envia BEGIN, chunks paralelos e COMMIT. Só confirma upload após ACK que inclui anúncio ao SP.

**Parâmetros**

- `const FileSession *session`: Identidade do Peer executor, FILE de progresso e cwd de origem da CLI.
- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.
- `const char *peer_host`: Endereço do Peer remoto que armazenará os chunks.
- `uint16_t peer_port`: Porta do armazenamento remoto.

**Passo a passo**

1. Valida o arquivo e calcula ObjectID e tamanho com leitura incremental. O arquivo não deve ser modificado durante a operação.
2. Monta metadados, prepara estado de trabalho e descobre o NodeID do armazenamento por PING.
3. Envia BEGIN para registrar o upload e distribui os índices no pool de workers.
4. Cada worker usa pread, SHA-256, LZ4 e STORE/CHUNK em sua própria conexão.
5. Depois de aguardar todos, envia COMMIT. O receptor verifica o documento e anuncia ao Super Peer antes do ACK.
6. Imprime hashes, métricas e conclusão no FILE da sessão. Limpa descritores, matrizes e sincronização ao sair.

**Funções do projeto usadas diretamente:** [`content_validate_pdf` (content.c)](#fn-content-c-content-validate-pdf); [`content_basename` (content.c)](#fn-content-c-content-basename); [`transfer_worker_count` (file_client.c)](#fn-file-client-c-transfer-worker-count); [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`discover_peer` (file_client.c)](#fn-file-client-c-discover-peer); [`execute_upload_workers` (file_client.c)](#fn-file-client-c-execute-upload-workers); [`object_id_file` (metadata.c)](#fn-metadata-c-object-id-file); [`object_id_to_hex` (metadata.c)](#fn-metadata-c-object-id-to-hex); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`transfer_encode_document` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-document); [`transfer_encode_object_operation` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-object-operation).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_command` (local_control.c)](#fn-local-control-c-handle-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EOVERFLOW`, `EBUSY`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de file_client_upload, para acompanhar a explicação</summary>

```c
int file_client_upload(const FileSession *session, const char *path, const char *peer_host, uint16_t peer_port)
{
    TransferDocument document;
    UploadWork work;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    Message response;
    char object_hex[OBJECT_ID_HEX_SIZE];
    int input_fd = -1;
    size_t worker_count;
    struct timespec started;
    struct timespec finished;
    double elapsed;
    int status = -1;

    fprintf(session->output, "State: CREATED\n");
    memset(&document, 0, sizeof(document));
    memset(&work, 0, sizeof(work));
    if (content_validate_pdf(path) < 0 || object_id_file(path, &document.id, &document.file_size) < 0 || content_basename(path, document.name) < 0 || document.file_size == 0U)
    {
        return -1;
    }
    document.chunk_count = document.file_size / METADATA_CHUNK_SIZE + (document.file_size % METADATA_CHUNK_SIZE != 0U);
    document.compression = COMPRESSION_LZ4;
    if (document.chunk_count > SIZE_MAX / OBJECT_ID_SIZE)
    {
        errno = EOVERFLOW;
        return -1;
    }
    input_fd = open(path, O_RDONLY);
    work.hashes = calloc((size_t)document.chunk_count, OBJECT_ID_SIZE);
    if (input_fd < 0 || work.hashes == NULL)
    {
        goto cleanup;
    }
    work.session = session;
    if (discover_peer(session, peer_host, peer_port, &work.destination) < 0) goto cleanup;
    work.input_fd = input_fd;
    work.host = peer_host;
    work.port = peer_port;
    work.document = document;
    work.state = TRANSFER_CREATED;
    if (pthread_mutex_init(&work.mutex, NULL) != 0)
    {
        errno = EBUSY;
        goto cleanup;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    if (transfer_encode_document(&document, &payload, &payload_size, TRANSFER_STORE_BEGIN) < 0 || request_expect(session, &work.destination, peer_host, peer_port, M_STORE, payload, payload_size, M_ACK, &response) < 0)
    {
        goto mutex_cleanup;
    }
    message_free(&response);
    free(payload);
    payload = NULL;
    worker_count = transfer_worker_count(document.chunk_count);
    work.state = TRANSFER_QUEUED;
    fprintf(session->output, "Transfer workers: %zu\n", worker_count);
    if (execute_upload_workers(&work, worker_count) < 0)
    {
        goto mutex_cleanup;
    }
    work.state = TRANSFER_VERIFYING;
    if (transfer_encode_object_operation(TRANSFER_STORE_COMMIT, &document.id, &payload, &payload_size) < 0 || request_expect(session, &work.destination, peer_host, peer_port, M_STORE, payload, payload_size, M_ACK, &response) < 0)
    {
        goto mutex_cleanup;
    }
    message_free(&response);
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    elapsed = (double)(finished.tv_sec - started.tv_sec) + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    if (object_id_to_hex(&document.id, object_hex, sizeof(object_hex)) < 0)
    {
        goto mutex_cleanup;
    }
    work.state = TRANSFER_FINISHED;
    fprintf(session->output, "File: %s\nSize: %" PRIu64 " bytes\nObjectID: %s\nChunks: %" PRIu64 "\n", document.name, document.file_size, object_hex, document.chunk_count);
    for (uint64_t index = 0U; index < document.chunk_count; ++index)
    {
        fprintf(session->output, "Chunk %" PRIu64 ": ", index);
        for (size_t byte = 0U; byte < OBJECT_ID_SIZE; ++byte)
        {
            fprintf(session->output, "%02x", (unsigned)work.hashes[(size_t)index * OBJECT_ID_SIZE + byte]);
        }
        fprintf(session->output, "\n");
    }
    fprintf(session->output, "Compression: LZ4\nOriginal bytes: %" PRIu64 "\nCompressed bytes: %" PRIu64 "\nDuration: %.3f s\nThroughput: %.2f MiB/s\nState: FINISHED\nUpload completed\n", document.file_size, work.compressed_total, elapsed, elapsed > 0.0 ? ((double)document.file_size / 1048576.0) / elapsed : 0.0);
    status = 0;

mutex_cleanup:
    free(payload);
    (void)pthread_mutex_destroy(&work.mutex);
cleanup:
    if (input_fd >= 0)
    {
        (void)close(input_fd);
    }
    free(work.hashes);
    return status;
}
```

</details>

<a id="fn-file-client-c-download-fail"></a>

#### download_fail

Fonte: `file_client.c:387` (linha nesta revisão; pode mudar em futuras edições).

```c
static void download_fail(void *context, int error);
```

Converte o contexto genérico para DownloadWork e registra o primeiro erro sob mutex. Não cancela pthreads à força: elas terminam suas atividades em andamento e deixam de buscar novas tarefas.

**Parâmetros**

- `void *context`: Contexto compartilhado entregue ao callback; seu tipo real é definido pelo módulo.
- `int error`: Código de erro a registrar ou codificar.

**Chamadores diretos encontrados nos fontes inventariados:** [`download_worker` (file_client.c)](#fn-file-client-c-download-worker).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de download_fail, para acompanhar a explicação</summary>

```c
static void download_fail(void *context, int error)
{
    DownloadWork *work = context;
    (void)pthread_mutex_lock(&work->mutex);
    if (!work->failed)
    {
        work->failed = 1;
        work->saved_errno = error == 0 ? EIO : error;
    }
    (void)pthread_mutex_unlock(&work->mutex);
}
```

</details>

<a id="fn-file-client-c-download-one"></a>

#### download_one

Fonte: `file_client.c:399` (linha nesta revisão; pode mudar em futuras edições).

```c
static int download_one(DownloadWork *work, uint64_t index);
```

pede o chunk aos endpoints anunciados, um por vez. Para cada resposta, confere descritor, offset/tamanho, descomprime LZ4, valida SHA-256 e usa pwrite_all; se um endpoint falha, tenta o próximo.

**Parâmetros**

- `DownloadWork *work`: Estado da transferência, com índices, erro, contadores e mutex.
- `uint64_t index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.

**Funções do projeto usadas diretamente:** [`compression_lz4_decompress` (compression.c)](#fn-compression-c-compression-lz4-decompress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`pwrite_all` (file_client.c)](#fn-file-client-c-pwrite-all); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`transfer_decode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-chunk); [`transfer_encode_chunk_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-chunk-request).

**Chamadores diretos encontrados nos fontes inventariados:** [`download_worker` (file_client.c)](#fn-file-client-c-download-worker).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EREMOTEIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de download_one, para acompanhar a explicação</summary>

```c
static int download_one(DownloadWork *work, uint64_t index)
{
    const TransferLookupResult *lookup = work->lookup;
    const TransferChunkLocations *locations = &lookup->chunks[index];
    uint8_t *request = NULL;
    uint32_t request_size = 0U;
    size_t endpoint_index;

    if (transfer_encode_chunk_request(&lookup->document.id, index, &request, &request_size) < 0)
    {
        return -1;
    }
    for (endpoint_index = 0U; endpoint_index < locations->peer_count; ++endpoint_index)
    {
        const TransferEndpoint *endpoint = &locations->peers[endpoint_index];
        Message response;
        TransferChunk chunk;
        uint8_t *raw = NULL;
        uint8_t hash[OBJECT_ID_SIZE];
        uint64_t expected_offset = index * METADATA_CHUNK_SIZE;
        uint64_t remaining = lookup->document.file_size - expected_offset;
        uint32_t expected_size = (uint32_t)(remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining);
        int valid = 0;

        fprintf(work->session->output, "Chunk %llu attempt %zu: %s:%u\n", (unsigned long long)index, endpoint_index + 1U, endpoint->ip, (unsigned)endpoint->port);
        if (request_expect(work->session, &endpoint->node_id, endpoint->ip, endpoint->port, M_DOWNLOAD_REQ, request, request_size, M_DOWNLOAD_REP, &response) == 0)
        {
            if (transfer_decode_chunk(response.payload, response.header.payload_size, TRANSFER_DOWNLOAD_CHUNK, &chunk) == 0 && memcmp(chunk.id.bytes, lookup->document.id.bytes, OBJECT_ID_SIZE) == 0 && chunk.index == index && chunk.offset == expected_offset && chunk.raw_size == expected_size && compression_lz4_decompress(chunk.data, chunk.compressed_size, chunk.raw_size, &raw) == 0 && content_sha256(raw, chunk.raw_size, hash) == 0 && memcmp(hash, chunk.hash, OBJECT_ID_SIZE) == 0 && memcmp(hash, locations->descriptor.hash, OBJECT_ID_SIZE) == 0 && pwrite_all(work->output_fd, raw, chunk.raw_size, chunk.offset) == 0)
            {
                valid = 1;
                (void)pthread_mutex_lock(&work->mutex);
                work->compressed_total += chunk.compressed_size;
                (void)pthread_mutex_unlock(&work->mutex);
            }
            free(raw);
            message_free(&response);
        }
        if (valid)
        {
            free(request);
            return 0;
        }
    }
    free(request);
    errno = EREMOTEIO;
    return -1;
}
```

</details>

<a id="fn-file-client-c-download-worker"></a>

#### download_worker

Fonte: `file_client.c:447` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *download_worker(void *argument);
```

distribui índices aos workers até acabar a fila ou ocorrer o primeiro erro. Chama download_one para cada chunk obtido.

**Parâmetros**

- `void *argument`: Contexto genérico de pthread/callback, convertido para a estrutura concreta na função.

**Funções do projeto usadas diretamente:** [`download_fail` (file_client.c)](#fn-file-client-c-download-fail); [`download_one` (file_client.c)](#fn-file-client-c-download-one).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de download_worker, para acompanhar a explicação</summary>

```c
static void *download_worker(void *argument)
{
    DownloadWork *work = argument;

    for (;;)
    {
        uint64_t index;

        (void)pthread_mutex_lock(&work->mutex);
        if (work->failed || work->next_index >= work->lookup->document.chunk_count)
        {
            (void)pthread_mutex_unlock(&work->mutex);
            break;
        }
        index = work->next_index++;
        work->state = TRANSFER_STARTED;
        (void)pthread_mutex_unlock(&work->mutex);
        (void)pthread_mutex_lock(&work->mutex);
        work->state = TRANSFER_TRANSFERRING;
        (void)pthread_mutex_unlock(&work->mutex);
        if (download_one(work, index) < 0)
        {
            download_fail(work, errno);
            break;
        }
    }
    return NULL;
}
```

</details>

<a id="fn-file-client-c-execute-download-workers"></a>

#### execute_download_workers

Fonte: `file_client.c:476` (linha nesta revisão; pode mudar em futuras edições).

```c
static int execute_download_workers(DownloadWork *work, size_t worker_count);
```

Usa execute_workers com os callbacks de download e, depois de aguardar as threads, converte work->error em -1/errno. A propriedade da estrutura de trabalho continua com file_client_download.

**Parâmetros**

- `DownloadWork *work`: Estado da transferência, com índices, erro, contadores e mutex.
- `size_t worker_count`: Quantidade de workers a iniciar para a transferência.

**Funções do projeto usadas diretamente:** [`execute_workers` (file_client.c)](#fn-file-client-c-execute-workers).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de execute_download_workers, para acompanhar a explicação</summary>

```c
static int execute_download_workers(DownloadWork *work, size_t worker_count)
{
    if (execute_workers(work, worker_count, download_worker, download_fail) < 0) return -1;
    if (work->failed) { errno = work->saved_errno; return -1; }
    return 0;
}
```

</details>

<a id="fn-file-client-c-lookup-document"></a>

#### lookup_document

Fonte: `file_client.c:483` (linha nesta revisão; pode mudar em futuras edições).

```c
static int lookup_document(const FileSession *session, const char *host, uint16_t port, const char *selector, TransferLookupResult *result);
```

Descobre identidade do SP por PING, envia LOOKUP com a identidade do serviço e decodifica metadados/localizações v2. Retorna resultado alocado para posterior liberação.

**Parâmetros**

- `const FileSession *session`: Identidade do Peer executor, FILE de progresso e cwd de origem da CLI.
- `const char *host`: Endereço IPv4 textual do endpoint remoto.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `const char *selector`: Nome de arquivo ou ObjectID hexadecimal que deve ser resolvido.
- `TransferLookupResult *result`: Estrutura de resultado de lookup, com documento e vetor de localizações; liberar por transfer_lookup_result_free.

**Funções do projeto usadas diretamente:** [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`discover_peer` (file_client.c)](#fn-file-client-c-discover-peer); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`transfer_encode_lookup_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-request); [`transfer_decode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-result).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de lookup_document, para acompanhar a explicação</summary>

```c
static int lookup_document(const FileSession *session, const char *host, uint16_t port, const char *selector, TransferLookupResult *result)
{
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    Message response;
    int status = -1;

    NodeID remote;

    if (discover_peer(session, host, port, &remote) < 0) return -1;
    if (transfer_encode_lookup_request(selector, &payload, &payload_size) < 0 || request_expect(session, &remote, host, port, M_LOOKUP, payload, payload_size, M_DOWNLOAD_REP, &response) < 0)
    {
        free(payload);
        return -1;
    }
    if (transfer_decode_lookup_result(response.payload, response.header.payload_size, result) == 0)
    {
        status = 0;
    }
    message_free(&response);
    free(payload);
    return status;
}
```

</details>

<a id="fn-file-client-c-file-client-download"></a>

#### file_client_download

Fonte: `file_client.c:507` (linha nesta revisão; pode mudar em futuras edições).

```c
int file_client_download(const FileSession *session, const char *selector, const char *destination, const char *superpeer_host, uint16_t superpeer_port);
```

Recebe a mesma sessão do Peer ativo; consulta o SP, cria .part exclusivo, busca chunks em paralelo e valida hashes contra o índice. Verifica ObjectID e publica por link exclusivo/fsync, sem sobrescrever destino.

**Parâmetros**

- `const FileSession *session`: Identidade do Peer executor, FILE de progresso e cwd de origem da CLI.
- `const char *selector`: Nome de arquivo ou ObjectID hexadecimal que deve ser resolvido.
- `const char *destination`: Caminho de destino do download; NULL seleciona o nome obtido no lookup.
- `const char *superpeer_host`: Endereço do Super Peer consultado ou usado para JOIN.
- `uint16_t superpeer_port`: Porta TCP do Super Peer.

**Passo a passo**

1. Consulta o Super Peer e obtém metadados, hashes esperados e endpoints por chunk.
2. Resolve o destino, recusa arquivo existente e cria destination.part com O_EXCL, sem tomar posse de um .part preexistente.
3. Ajusta o tamanho do arquivo e inicia workers que usam pwrite em regiões distintas.
4. Cada worker pode tentar várias localizações; dados só são escritos depois de validação do descritor, descompressão e SHA.
5. Após as threads, sincroniza o arquivo e calcula ObjectID/tamanho do .part completo.
6. Publica por link exclusivo e sincroniza o diretório; remove .part e sincroniza novamente. O uso de link impede sobrescrever um destino criado durante a operação.
7. Em erro, remove apenas o .part que esta chamada criou e libera lookup/buffers. Uma falha após link pode deixar destino publicado, sem retorno de sucesso.

**Funções do projeto usadas diretamente:** [`content_sync_parent` (content.c)](#fn-content-c-content-sync-parent); [`transfer_worker_count` (file_client.c)](#fn-file-client-c-transfer-worker-count); [`execute_download_workers` (file_client.c)](#fn-file-client-c-execute-download-workers); [`lookup_document` (file_client.c)](#fn-file-client-c-lookup-document); [`object_id_file` (metadata.c)](#fn-metadata-c-object-id-file); [`transfer_lookup_result_free` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-lookup-result-free).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_command` (local_control.c)](#fn-local-control-c-handle-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`, `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de file_client_download, para acompanhar a explicação</summary>

```c
int file_client_download(const FileSession *session, const char *selector, const char *destination, const char *superpeer_host, uint16_t superpeer_port)
{
    TransferLookupResult lookup;
    DownloadWork work;
    ObjectID downloaded_id;
    uint64_t downloaded_size;
    char *part_path = NULL;
    int output_fd = -1;
    int owns_part = 0;
    char default_path[4096];
    size_t destination_size;
    size_t worker_count;
    struct timespec started;
    struct timespec finished;
    double elapsed;
    int status = -1;

    memset(&lookup, 0, sizeof(lookup));
    memset(&work, 0, sizeof(work));
    if (lookup_document(session, superpeer_host, superpeer_port, selector, &lookup) < 0)
    {
        return -1;
    }
    if (destination == NULL)
    {
        if (strchr(lookup.document.name, '/') != NULL || strcmp(lookup.document.name, ".") == 0 || strcmp(lookup.document.name, "..") == 0)
        {
            errno = EBADMSG;
            goto cleanup;
        }
        int length = snprintf(default_path, sizeof(default_path), "%s/%s", session->directory, lookup.document.name);
        if (length < 0 || (size_t)length >= sizeof(default_path)) { errno = ENAMETOOLONG; goto cleanup; }
        destination = default_path;
    }
    destination_size = strlen(destination);
    if (destination_size == 0U || destination_size > SIZE_MAX - PART_PATH_EXTRA || access(destination, F_OK) == 0)
    {
        errno = access(destination, F_OK) == 0 ? EEXIST : EINVAL;
        goto cleanup;
    }
    part_path = malloc(destination_size + PART_PATH_EXTRA);
    if (part_path == NULL)
    {
        goto cleanup;
    }
    (void)snprintf(part_path, destination_size + PART_PATH_EXTRA, "%s.part", destination);
    output_fd = open(part_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output_fd < 0)
    {
        goto cleanup;
    }
    owns_part = 1;
    if (lookup.document.file_size > (uint64_t)INT64_MAX || ftruncate(output_fd, (off_t)lookup.document.file_size) < 0 || pthread_mutex_init(&work.mutex, NULL) != 0)
    {
        goto cleanup;
    }
    work.session = session;
    work.output_fd = output_fd;
    work.lookup = &lookup;
    work.state = TRANSFER_CREATED;
    worker_count = transfer_worker_count(lookup.document.chunk_count);
    work.state = TRANSFER_QUEUED;
    fprintf(session->output, "Transfer workers: %zu\n", worker_count);
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    if (execute_download_workers(&work, worker_count) < 0)
    {
        (void)pthread_mutex_destroy(&work.mutex);
        goto cleanup;
    }
    work.state = TRANSFER_VERIFYING;
    if (fsync(output_fd) < 0 || object_id_file(part_path, &downloaded_id, &downloaded_size) < 0 || downloaded_size != lookup.document.file_size || memcmp(downloaded_id.bytes, lookup.document.id.bytes, OBJECT_ID_SIZE) != 0 || link(part_path, destination) < 0 || content_sync_parent(destination) < 0 || unlink(part_path) < 0 || content_sync_parent(destination) < 0)
    {
        (void)pthread_mutex_destroy(&work.mutex);
        goto cleanup;
    }
    (void)pthread_mutex_destroy(&work.mutex);
    work.state = TRANSFER_FINISHED;
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    elapsed = (double)(finished.tv_sec - started.tv_sec) + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    fprintf(session->output, "Original bytes: %" PRIu64 "\nCompressed bytes: %" PRIu64 "\nDuration: %.3f s\nThroughput: %.2f MiB/s\nState: FINISHED\nDownload completed\nSHA-256 verified\n", lookup.document.file_size, work.compressed_total, elapsed, elapsed > 0.0 ? ((double)lookup.document.file_size / 1048576.0) / elapsed : 0.0);
    status = 0;

cleanup:
    if (output_fd >= 0)
    {
        (void)close(output_fd);
    }
    if (status < 0 && owns_part)
    {
        (void)unlink(part_path);
    }
    free(part_path);
    transfer_lookup_result_free(&lookup);
    return status;
}
```

</details>

<a id="fn-file-client-c-file-client-benchmark-lz4"></a>

#### file_client_benchmark_lz4

Fonte: `file_client.c:603` (linha nesta revisão; pode mudar em futuras edições).

```c
int file_client_benchmark_lz4(const char *path);
```

percorre um PDF em chunks e mede apenas a compressão LZ4; informa bytes, duração e taxa. Não é teste rígido de desempenho nem verifica rede.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Funções do projeto usadas diretamente:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`content_validate_pdf` (content.c)](#fn-content-c-content-validate-pdf); [`pread_all` (file_client.c)](#fn-file-client-c-pread-all).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (peer.c)](#fn-peer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de file_client_benchmark_lz4, para acompanhar a explicação</summary>

```c
int file_client_benchmark_lz4(const char *path)
{
    uint8_t *raw = NULL;
    int fd = -1;
    struct stat information;
    struct timespec started;
    struct timespec finished;
    uint64_t offset = 0U;
    uint64_t compressed_total = 0U;
    double elapsed;
    int status = -1;

    if (content_validate_pdf(path) < 0 || stat(path, &information) < 0 || information.st_size <= 0)
    {
        return -1;
    }
    raw = malloc((size_t)METADATA_CHUNK_SIZE);
    fd = open(path, O_RDONLY);
    if (raw == NULL || fd < 0)
    {
        goto cleanup;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    while (offset < (uint64_t)information.st_size)
    {
        uint64_t remaining = (uint64_t)information.st_size - offset;
        size_t size = remaining > METADATA_CHUNK_SIZE ? (size_t)METADATA_CHUNK_SIZE : (size_t)remaining;
        uint8_t *compressed = NULL;
        size_t compressed_size = 0U;

        if (pread_all(fd, raw, size, offset) < 0 || compression_lz4_compress(raw, size, &compressed, &compressed_size) < 0)
        {
            free(compressed);
            goto cleanup;
        }
        compressed_total += compressed_size;
        offset += size;
        free(compressed);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    elapsed = (double)(finished.tv_sec - started.tv_sec) + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    printf("LZ4 benchmark (informativo)\nOriginal bytes: %" PRIu64 "\nCompressed bytes: %" PRIu64 "\nDuration: %.6f s\nCompression throughput: %.2f MiB/s\n", offset, compressed_total, elapsed, elapsed > 0.0 ? ((double)offset / 1048576.0) / elapsed : 0.0);
    status = 0;

cleanup:
    if (fd >= 0)
    {
        (void)close(fd);
    }
    free(raw);
    return status;
}
```

</details>

<a id="mod-local-control-c"></a>

### local_control.c

[control_path](#fn-local-control-c-control-path) · [send_string](#fn-local-control-c-send-string) · [receive_string](#fn-local-control-c-receive-string) · [progress_write](#fn-local-control-c-progress-write) · [handle_command](#fn-local-control-c-handle-command) · [control_loop](#fn-local-control-c-control-loop) · [local_control_start](#fn-local-control-c-local-control-start) · [local_control_stop](#fn-local-control-c-local-control-stop) · [absolute_path](#fn-local-control-c-absolute-path) · [local_control_command](#fn-local-control-c-local-control-command)

<a id="fn-local-control-c-control-path"></a>

#### control_path

Fonte: `local_control.c:34` (linha nesta revisão; pode mudar em futuras edições).

```c
static int control_path(uint16_t port, char path[108]);
```

constrói diretório privado por UID e verifica tipo, dono e permissões antes de usar o socket da porta.

**Parâmetros**

- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `char path[108]`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Chamadores diretos encontrados nos fontes inventariados:** [`local_control_start` (local_control.c)](#fn-local-control-c-local-control-start); [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EACCES`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de control_path, para acompanhar a explicação</summary>

```c
static int control_path(uint16_t port, char path[108])
{
    char directory[80];
    struct stat info;
    (void)snprintf(directory, sizeof(directory), "/tmp/pd-c2-%lu", (unsigned long)getuid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(directory, &info) < 0) return -1;
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077U) != 0U) { errno = EACCES; return -1; }
    (void)snprintf(path, 108U, "%s/peer-%u.sock", directory, (unsigned)port);
    return 0;
}
```

</details>

<a id="fn-local-control-c-send-string"></a>

#### send_string

Fonte: `local_control.c:46` (linha nesta revisão; pode mudar em futuras edições).

```c
static int send_string(int fd, const char *text);
```

Obtém o comprimento do texto e envia um uint32 big-endian seguido dos bytes, sem NUL na rede. Texto nulo é representado como comprimento zero. Usa envio completo; o ponteiro recebido é emprestado.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.

**Funções do projeto usadas diretamente:** [`network_send_all` (network.c)](#fn-network-c-network-send-all).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_command` (local_control.c)](#fn-local-control-c-handle-command); [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command).

**Retorno:** as expressões presentes nesta função são `-1`, `network_send_all(fd, &length, sizeof(length)) == (ssize_t)sizeof(length) && network_send_all(fd, text, size) == (ssize_t)size ? 0 : -1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EOVERFLOW`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de send_string, para acompanhar a explicação</summary>

```c
static int send_string(int fd, const char *text)
{
    size_t size = strlen(text);
    uint32_t length;
    if (size > CONTROL_LIMIT) { errno = EOVERFLOW; return -1; }
    length = htonl((uint32_t)size);
    return network_send_all(fd, &length, sizeof(length)) == (ssize_t)sizeof(length) && network_send_all(fd, text, size) == (ssize_t)size ? 0 : -1;
}
```

</details>

<a id="fn-local-control-c-receive-string"></a>

#### receive_string

Fonte: `local_control.c:55` (linha nesta revisão; pode mudar em futuras edições).

```c
static int receive_string(int fd, char **output, uint32_t limit);
```

Recebe o uint32 de comprimento, exige tamanho dentro do limite informado e aloca comprimento+1. Recebe todos os bytes, rejeita NUL interno e acrescenta o terminador local. O chamador libera a string; em falha a alocação é desfeita.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `char **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t limit`: Maior comprimento aceito para a string recebida.

**Funções do projeto usadas diretamente:** [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_command` (local_control.c)](#fn-local-control-c-handle-command); [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EMSGSIZE`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de receive_string, para acompanhar a explicação</summary>

```c
static int receive_string(int fd, char **output, uint32_t limit)
{
    uint32_t length;
    char *text;
    *output = NULL;
    if (network_recv_exact(fd, &length, sizeof(length)) != (ssize_t)sizeof(length)) return -1;
    length = ntohl(length);
    if (length > limit) { errno = EMSGSIZE; return -1; }
    text = calloc((size_t)length + 1U, 1U);
    if (text == NULL) return -1;
    if (network_recv_exact(fd, text, length) != (ssize_t)length || memchr(text, 0, length) != NULL) { free(text); errno = EBADMSG; return -1; }
    *output = text;
    return 0;
}
```

</details>

<a id="fn-local-control-c-progress-write"></a>

#### progress_write

Fonte: `local_control.c:71` (linha nesta revisão; pode mudar em futuras edições).

```c
static ssize_t progress_write(void *cookie, const char *buffer, size_t size);
```

callback de FILE criado com fopencookie; transforma escrita em frame de progresso UINT32_MAX mais comprimento/dados. Não redireciona stdout global.

**Parâmetros**

- `void *cookie`: Contexto do FILE customizado, usado pelo callback para acessar o canal local.
- `const char *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Funções do projeto usadas diretamente:** [`network_send_all` (network.c)](#fn-network-c-network-send-all).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Retorno:** as expressões presentes nesta função são `-1`, `(ssize_t)size`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EMSGSIZE`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de progress_write, para acompanhar a explicação</summary>

```c
static ssize_t progress_write(void *cookie, const char *buffer, size_t size)
{
    int fd = *(int *)cookie;
    if (size > CONTROL_LIMIT) { errno = EMSGSIZE; return -1; }
    uint32_t header[2] = {htonl(UINT32_MAX), htonl((uint32_t)size)};
    if (network_send_all(fd, header, sizeof(header)) != (ssize_t)sizeof(header) || network_send_all(fd, buffer, size) != (ssize_t)size) return -1;
    return (ssize_t)size;
}
```

</details>

<a id="fn-local-control-c-handle-command"></a>

#### handle_command

Fonte: `local_control.c:80` (linha nesta revisão; pode mudar em futuras edições).

```c
static void handle_command(LocalControl *control, int fd);
```

recebe operação, porta e cinco strings; cria sessão com NodeID do serviço e stream de progresso; chama upload/download; envia resultado final e libera todos os campos.

**Parâmetros**

- `LocalControl *control`: Estado do canal Unix, incluindo identidade do executor e descritores locais.
- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.

**Passo a passo**

1. Recebe operação, porta e cinco strings com limites explícitos.
2. Valida campos e cria um FILE de progresso usando fopencookie; sua escrita chama progress_write.
3. Monta FileSession com o NodeID do serviço, não com dados arbitrários de identidade enviados pela CLI.
4. Executa upload ou download de forma síncrona nesse laço local; os chunks internos usam threads.
5. Fecha o stream, envia status final e libera todas as strings alocadas.

**Funções do projeto usadas diretamente:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download); [`send_string` (local_control.c)](#fn-local-control-c-send-string); [`receive_string` (local_control.c)](#fn-local-control-c-receive-string); [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact).

**Chamadores diretos encontrados nos fontes inventariados:** [`control_loop` (local_control.c)](#fn-local-control-c-control-loop).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `fopencookie`: Cria um FILE com callback de escrita personalizado; o progresso é enviado pelo socket sem trocar stdout global.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de handle_command, para acompanhar a explicação</summary>

```c
static void handle_command(LocalControl *control, int fd)
{
    uint32_t header[2];
    char *fields[5] = {NULL};
    FILE *stream = NULL;
    int result = -1;
    int error = EBADMSG;
    if (network_recv_exact(fd, header, sizeof(header)) != (ssize_t)sizeof(header)) goto cleanup;
    for (size_t i = 0U; i < 5U; ++i) if (receive_string(fd, &fields[i], 4095U) < 0) goto cleanup;
    if (ntohl(header[0]) > 1U || ntohl(header[1]) == 0U || ntohl(header[1]) > UINT16_MAX || fields[4][0] != '/' || fields[0][0] == '\0') goto cleanup;
    cookie_io_functions_t io = {.write = progress_write};
    stream = fopencookie(&fd, "w", io);
    if (stream != NULL) setvbuf(stream, NULL, _IOLBF, 0U);
    if (stream == NULL) { error = errno; goto cleanup; }
    FileSession session = {.source = control->source, .output = stream, .directory = fields[4]};
    if (ntohl(header[0]) == 1U)
        result = file_client_upload(&session, fields[0], fields[2], (uint16_t)ntohl(header[1]));
    else
        result = file_client_download(&session, fields[0], fields[1][0] == '\0' ? NULL : fields[1], fields[2], (uint16_t)ntohl(header[1]));
    error = result == 0 ? 0 : (errno == 0 ? EIO : errno);
    if (error != 0) (void)fprintf(stream, "Operação rejeitada: %s\n", strerror(error));
    if (fclose(stream) != 0 && error == 0) error = EIO;
    stream = NULL;
cleanup:
    header[0] = htonl((uint32_t)error);
    (void)network_send_all(fd, header, sizeof(header[0]));
    (void)send_string(fd, error == EBADMSG ? "Comando local inválido\n" : "");
    if (stream != NULL) fclose(stream);
    for (size_t i = 0U; i < 5U; ++i) free(fields[i]);
}
```

</details>

<a id="fn-local-control-c-control-loop"></a>

#### control_loop

Fonte: `local_control.c:111` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *control_loop(void *argument);
```

aceita um comando local por vez, registra descritor ativo sob mutex, atende e fecha. Transferências internas continuam paralelas.

**Parâmetros**

- `void *argument`: Contexto genérico de pthread/callback, convertido para a estrutura concreta na função.

**Funções do projeto usadas diretamente:** [`handle_command` (local_control.c)](#fn-local-control-c-handle-command).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `accept`: Cria um descritor conectado separado do listener. Cada conexão tem vida própria.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de control_loop, para acompanhar a explicação</summary>

```c
static void *control_loop(void *argument)
{
    LocalControl *control = argument;
    for (;;)
    {
        int fd = accept(control->listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pthread_mutex_lock(&control->mutex);
        if (control->stopping) { pthread_mutex_unlock(&control->mutex); close(fd); break; }
        control->client = fd;
        pthread_mutex_unlock(&control->mutex);
        handle_command(control, fd);
        pthread_mutex_lock(&control->mutex);
        control->client = -1;
        close(fd);
        pthread_mutex_unlock(&control->mutex);
    }
    return NULL;
}
```

</details>

<a id="fn-local-control-c-local-control-start"></a>

#### local_control_start

Fonte: `local_control.c:131` (linha nesta revisão; pode mudar em futuras edições).

```c
int local_control_start(uint16_t port, const NodeID *source, LocalControl **output);
```

adquire flock exclusivo, remove somente socket antigo protegido pelo lock, cria AF_UNIX com permissão 0600 e inicia a thread de controle.

**Parâmetros**

- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `const NodeID *source`: NodeID da origem da RPC; nulo significa identidade não informada no modo legado.
- `LocalControl **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Funções do projeto usadas diretamente:** [`control_path` (local_control.c)](#fn-local-control-c-control-path).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.
- `pthread_create`: Inicia thread com callback e contexto; erro vem no retorno pthread, não necessariamente em errno.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de local_control_start, para acompanhar a explicação</summary>

```c
int local_control_start(uint16_t port, const NodeID *source, LocalControl **output)
{
    LocalControl *control = calloc(1U, sizeof(*control));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char lock_path[120];
    int error;
    if (control == NULL) return -1;
    *output = NULL;
    control->listener = -1;
    control->lock = -1;
    control->client = -1;
    control->source = *source;
    if (control_path(port, control->path) < 0) goto fail;
    (void)snprintf(lock_path, sizeof(lock_path), "%s.lock", control->path);
    control->lock = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (control->lock < 0 || flock(control->lock, LOCK_EX | LOCK_NB) < 0) goto fail;
    control->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (control->listener < 0) goto fail;
    strcpy(address.sun_path, control->path);
    (void)unlink(control->path); /* Lock exclusivo permite remover somente um socket antigo deste Peer. */
    if (bind(control->listener, (struct sockaddr *)&address, sizeof(address)) < 0 || chmod(control->path, 0600) < 0 || listen(control->listener, 64) < 0) goto fail;
    error = pthread_mutex_init(&control->mutex, NULL);
    if (error != 0) { errno = error; goto fail; }
    error = pthread_create(&control->thread, NULL, control_loop, control);
    if (error != 0) { pthread_mutex_destroy(&control->mutex); errno = error; goto fail; }
    *output = control;
    return 0;
fail:
    error = errno;
    if (control->listener >= 0) close(control->listener);
    if (control->lock >= 0) close(control->lock);
    free(control);
    errno = error;
    return -1;
}
```

</details>

<a id="fn-local-control-c-local-control-stop"></a>

#### local_control_stop

Fonte: `local_control.c:167` (linha nesta revisão; pode mudar em futuras edições).

```c
void local_control_stop(LocalControl *control);
```

sinaliza stopping, interrompe sockets local/listener, aguarda thread e remove socket. Não apaga storage.

**Parâmetros**

- `LocalControl *control`: Estado do canal Unix, incluindo identidade do executor e descritores locais.

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `pthread_join`: Aguarda a thread, garantindo que o contexto dela não seja liberado enquanto ainda estiver em uso.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `unlink`: Remove um nome de arquivo/socket. Não é exclusão recursiva e não deve atingir arquivo que a operação não criou.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de local_control_stop, para acompanhar a explicação</summary>

```c
void local_control_stop(LocalControl *control)
{
    if (control == NULL) return;
    pthread_mutex_lock(&control->mutex);
    control->stopping = 1;
    if (control->client >= 0) shutdown(control->client, SHUT_RDWR);
    pthread_mutex_unlock(&control->mutex);
    shutdown(control->listener, SHUT_RDWR);
    pthread_join(control->thread, NULL);
    close(control->listener);
    unlink(control->path);
    close(control->lock);
    pthread_mutex_destroy(&control->mutex);
    free(control);
}
```

</details>

<a id="fn-local-control-c-absolute-path"></a>

#### absolute_path

Fonte: `local_control.c:183` (linha nesta revisão; pode mudar em futuras edições).

```c
static int absolute_path(const char *path, const char *directory, char output[PATH_MAX]);
```

combina caminho relativo com cwd da CLI sem alterar cwd do serviço.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.
- `const char *directory`: Adaptador de metadados/membership quando Directory*; em const char*, caminho de diretório.
- `char output[PATH_MAX]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Chamadores diretos encontrados nos fontes inventariados:** [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de absolute_path, para acompanhar a explicação</summary>

```c
static int absolute_path(const char *path, const char *directory, char output[PATH_MAX])
{
    int length = snprintf(output, PATH_MAX, "%s%s%s", path[0] == '/' ? "" : directory, path[0] == '/' ? "" : "/", path);
    if (length < 0 || length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
```

</details>

<a id="fn-local-control-c-local-control-command"></a>

#### local_control_command

Fonte: `local_control.c:190` (linha nesta revisão; pode mudar em futuras edições).

```c
int local_control_command(uint16_t local_port, int upload, const char *file, const char *destination, const char *host, uint16_t remote_port);
```

conecta ao Peer escolhido, envia campos e imprime frames até o resultado final. Aguarda progresso sem prazo total arbitrário; fechamento do serviço encerra a espera. Não cria conexão TCP ao SP.

**Parâmetros**

- `uint16_t local_port`: Porta do serviço local e de identificação de seu socket Unix.
- `int upload`: Seleciona upload (não zero) ou download (zero) no canal local.
- `const char *file`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `const char *destination`: Caminho de destino do download; NULL seleciona o nome obtido no lookup.
- `const char *host`: Endereço IPv4 textual do endpoint remoto.
- `uint16_t remote_port`: Porta do endpoint remoto, não a escolha do executor local.

**Passo a passo**

1. Resolve caminhos relativos usando o cwd da CLI; não muda o diretório global do serviço.
2. Abre AF_UNIX para a porta lógica local e envia operação, porta remota e campos textuais delimitados por tamanho.
3. O serviço recebe o pedido e usa a sua própria identidade; não é enviado um NodeID inventado pela CLI.
4. Lê frames de progresso marcados por UINT32_MAX e imprime seu conteúdo.
5. O frame final contém status; zero significa sucesso e o erro local é devolvido por errno. Fecha o canal em todos os caminhos.

**Funções do projeto usadas diretamente:** [`control_path` (local_control.c)](#fn-local-control-c-control-path); [`send_string` (local_control.c)](#fn-local-control-c-send-string); [`receive_string` (local_control.c)](#fn-local-control-c-receive-string); [`absolute_path` (local_control.c)](#fn-local-control-c-absolute-path); [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact).

**Chamadores diretos encontrados nos fontes inventariados:** [`option_command` (peer.c)](#fn-peer-c-option-command); [`main` (peer.c)](#fn-peer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `poll`: Espera prontidão em descritores por prazo limitado. Prontidão deve ser seguida da operação que confirma o resultado.
- `socket`: Cria um endpoint local; por si só não faz bind, listen ou connect.

**Retorno:** as expressões presentes nesta função são `-1`, `result`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de local_control_command, para acompanhar a explicação</summary>

```c
int local_control_command(uint16_t local_port, int upload, const char *file, const char *destination, const char *host, uint16_t remote_port)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    char cwd[PATH_MAX], source[PATH_MAX], target[PATH_MAX] = "";
    uint32_t header[2] = {htonl(upload ? 1U : 0U), htonl(remote_port)};
    char *report = NULL;
    int result = -1;
    int fd;
    if (getcwd(cwd, sizeof(cwd)) == NULL || control_path(local_port, address.sun_path) < 0) return -1;
    if (upload && absolute_path(file, cwd, source) < 0) return -1;
    if (destination != NULL && absolute_path(destination, cwd, target) < 0) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        fprintf(stderr, "Peer local %u indisponível; inicie peer serve ou selecione --local-peer-port.\n", (unsigned)local_port);
        goto cleanup;
    }
    if (network_send_all(fd, header, sizeof(header)) != (ssize_t)sizeof(header) || send_string(fd, upload ? source : file) < 0 || send_string(fd, target) < 0 || send_string(fd, host) < 0 || send_string(fd, "") < 0 || send_string(fd, cwd) < 0) goto cleanup;
    for (;;)
    {
        /* A operação pode levar minutos; EOF detecta encerramento do serviço. */
        struct pollfd waiting = {.fd = fd, .events = POLLIN};
        int ready;
        do { ready = poll(&waiting, 1U, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0 || network_recv_exact(fd, header, sizeof(header[0])) != (ssize_t)sizeof(header[0]) || receive_string(fd, &report, CONTROL_LIMIT) < 0) goto cleanup;
        fputs(report, stdout);
        fflush(stdout);
        free(report);
        report = NULL;
        if (ntohl(header[0]) != UINT32_MAX) break;
    }
    errno = (int)ntohl(header[0]);
    result = errno == 0 ? 0 : -1;
cleanup:
    { int error = errno; free(report); close(fd); errno = error; }
    return result;
}
```

</details>

<a id="mod-membership-c"></a>

### membership.c

[lock_members](#fn-membership-c-lock-members) · [unlock_members](#fn-membership-c-unlock-members) · [find_member_index_locked](#fn-membership-c-find-member-index-locked) · [grow_members_locked](#fn-membership-c-grow-members-locked) · [set_member](#fn-membership-c-set-member) · [superpeer_config_init_with_uuid](#fn-membership-c-superpeer-config-init-with-uuid) · [superpeer_config_init](#fn-membership-c-superpeer-config-init) · [superpeer_create](#fn-membership-c-superpeer-create) · [superpeer_destroy](#fn-membership-c-superpeer-destroy) · [superpeer_get_node](#fn-membership-c-superpeer-get-node) · [superpeer_register_node](#fn-membership-c-superpeer-register-node) · [superpeer_unregister_node](#fn-membership-c-superpeer-unregister-node) · [superpeer_find_member](#fn-membership-c-superpeer-find-member) · [superpeer_member_count](#fn-membership-c-superpeer-member-count) · [superpeer_is_registered](#fn-membership-c-superpeer-is-registered)

<a id="fn-membership-c-lock-members"></a>

#### lock_members

Fonte: `membership.c:22` (linha nesta revisão; pode mudar em futuras edições).

```c
static int lock_members(SuperPeer *superpeer);
```

Adquire o mutex da tabela de membros. Traduz o código de erro pthread para errno/-1. Não executa a busca; apenas estabelece a região exclusiva para as operações seguintes.

**Parâmetros**

- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_unregister_node` (membership.c)](#fn-membership-c-superpeer-unregister-node); [`superpeer_find_member` (membership.c)](#fn-membership-c-superpeer-find-member); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`superpeer_is_registered` (membership.c)](#fn-membership-c-superpeer-is-registered).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de lock_members, para acompanhar a explicação</summary>

```c
static int lock_members(SuperPeer *superpeer)
{
    int error = pthread_mutex_lock(&superpeer->members_mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-membership-c-unlock-members"></a>

#### unlock_members

Fonte: `membership.c:35` (linha nesta revisão; pode mudar em futuras edições).

```c
static int unlock_members(SuperPeer *superpeer);
```

Libera o mutex da tabela de membros e converte eventual erro pthread em errno/-1. Só deve ser usado por quem adquiriu esse mutex.

**Parâmetros**

- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_unregister_node` (membership.c)](#fn-membership-c-superpeer-unregister-node); [`superpeer_find_member` (membership.c)](#fn-membership-c-superpeer-find-member); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`superpeer_is_registered` (membership.c)](#fn-membership-c-superpeer-is-registered).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de unlock_members, para acompanhar a explicação</summary>

```c
static int unlock_members(SuperPeer *superpeer)
{
    int error = pthread_mutex_unlock(&superpeer->members_mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-membership-c-find-member-index-locked"></a>

#### find_member_index_locked

Fonte: `membership.c:48` (linha nesta revisão; pode mudar em futuras edições).

```c
static int find_member_index_locked(const SuperPeer *superpeer, const NodeID *node_id, size_t *index);
```

busca linear por NodeID. O sufixo locked avisa que o chamador já precisa possuir o mutex; quando index é não nulo, devolve a posição encontrada.

**Parâmetros**

- `const SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `const NodeID *node_id`: Identificador binário de nó, com 32 bytes.
- `size_t *index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.

**Funções do projeto usadas diretamente:** [`node_id_equal` (node.c)](#fn-node-c-node-id-equal).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_unregister_node` (membership.c)](#fn-membership-c-superpeer-unregister-node); [`superpeer_find_member` (membership.c)](#fn-membership-c-superpeer-find-member); [`superpeer_is_registered` (membership.c)](#fn-membership-c-superpeer-is-registered).

**Retorno:** as expressões presentes nesta função são `0`, `-1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de find_member_index_locked, para acompanhar a explicação</summary>

```c
static int find_member_index_locked(const SuperPeer *superpeer, const NodeID *node_id, size_t *index)
{
    size_t i;
    // O index é opcional, então não precisamos inicializá-lo aqui.
    for (i = 0U; i < superpeer->member_count; ++i)
    {
        if (node_id_equal(&superpeer->members[i].node.id, node_id))
        {
            if (index != NULL)
            {
                *index = i;
            }
            return 0;
        }
    }
    return -1;
}
```

</details>

<a id="fn-membership-c-grow-members-locked"></a>

#### grow_members_locked

Fonte: `membership.c:67` (linha nesta revisão; pode mudar em futuras edições).

```c
static int grow_members_locked(SuperPeer *superpeer);
```

dobra o vetor quando cheio, começando da capacidade padrão. Verifica overflow antes de realloc; em falha, o vetor antigo permanece válido.

**Parâmetros**

- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `realloc`: Pode mover uma alocação. Se falhar, a alocação antiga permanece válida; observe o uso de ponteiro temporário.

**Retorno:** as expressões presentes nesta função são `0`, `-1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENOMEM`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de grow_members_locked, para acompanhar a explicação</summary>

```c
static int grow_members_locked(SuperPeer *superpeer)
{
    size_t new_capacity;
    SuperPeerMember *new_members;

    // Se ainda há espaço, não é necessário crescer.
    if (superpeer->member_count < superpeer->member_capacity)
    {
        return 0;
    }

    // Se a capacidade atual for zero, inicializamos com a capacidade padrão. Caso contrário, dobramos a capacidade.
    if (superpeer->member_capacity == 0U)
    {
        new_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    }
    else
    {
        if (superpeer->member_capacity > SIZE_MAX / 2U)
        {
            errno = ENOMEM;
            return -1;
        }
        new_capacity = superpeer->member_capacity * 2U;
    }

    // Verifica se a nova capacidade multiplicada pelo tamanho do elemento não causa overflow.
    if (new_capacity > SIZE_MAX / sizeof(*new_members))
    {
        errno = ENOMEM;
        return -1;
    }

    // Realoca a memória para os membros com a nova capacidade.
    new_members = realloc(superpeer->members, new_capacity * sizeof(*new_members));
    if (new_members == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    superpeer->members = new_members;
    superpeer->member_capacity = new_capacity;
    return 0;
}
```

</details>

<a id="fn-membership-c-set-member"></a>

#### set_member

Fonte: `membership.c:114` (linha nesta revisão; pode mudar em futuras edições).

```c
static void set_member(SuperPeerMember *member, const Node *node);
```

copia os dados do nó e atualiza ALIVE/last_seen. last_seen é um registro local de atualização, **não** prova de heartbeat implementado.

**Parâmetros**

- `SuperPeerMember *member`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `const Node *node`: Estrutura que reúne identidade, configuração, PID e papel de um nó.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de set_member, para acompanhar a explicação</summary>

```c
static void set_member(SuperPeerMember *member, const Node *node)
{
    member->node = *node;
    member->state = SUPERPEER_MEMBER_ALIVE;
    member->last_seen = time(NULL);
}
```

</details>

<a id="fn-membership-c-superpeer-config-init-with-uuid"></a>

#### superpeer_config_init_with_uuid

Fonte: `membership.c:122` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_config_init_with_uuid(SuperPeerConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE]);
```

monta uma configuração com UUID fornecido e capacidade inicial padrão. Usado quando se quer identidade reprodutível.

**Parâmetros**

- `SuperPeerConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `const uint8_t uuid[NODE_UUID_SIZE]`: 16 bytes persistentes usados como componente da identidade.

**Funções do projeto usadas diretamente:** [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid).

**Chamadores diretos encontrados nos fontes inventariados:** [`initialize_local_identity` (superpeer_app.c)](#fn-superpeer-app-c-initialize-local-identity); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`test_superpeer_members_are_thread_safe` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_config_init_with_uuid, para acompanhar a explicação</summary>

```c
int superpeer_config_init_with_uuid(SuperPeerConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE])
{
    // Valida parâmetros de entrada.
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    // Configura o nó com o UUID fornecido.
    if (node_config_init_with_uuid(&config->node, ip, port, uuid) == -1)
    {
        return -1;
    }

    config->initial_member_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    return 0;
}
```

</details>

<a id="fn-membership-c-superpeer-config-init"></a>

#### superpeer_config_init

Fonte: `membership.c:141` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_config_init(SuperPeerConfig *config, const char *ip, uint16_t port);
```

semelhante, mas gera UUID novo.

**Parâmetros**

- `SuperPeerConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.

**Funções do projeto usadas diretamente:** [`node_config_init` (node.c)](#fn-node-c-node-config-init).

**Chamadores diretos encontrados nos fontes inventariados:** [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_config_init, para acompanhar a explicação</summary>

```c
int superpeer_config_init(SuperPeerConfig *config, const char *ip, uint16_t port)
{
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_init(&config->node, ip, port) == -1)
    {
        return -1;
    }

    config->initial_member_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    return 0;
}
```

</details>

<a id="fn-membership-c-superpeer-create"></a>

#### superpeer_create

Fonte: `membership.c:158` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_create(const SuperPeerConfig *config, SuperPeer **output);
```

valida configuração, inicializa nó local com papel SUPERPEER, aloca vetor/mutex e registra o próprio nó como membro 0. O chamador passa a ser dono do objeto resultante.

**Parâmetros**

- `const SuperPeerConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `SuperPeer **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Funções do projeto usadas diretamente:** [`set_member` (membership.c)](#fn-membership-c-set-member); [`node_config_validate` (node.c)](#fn-node-c-node-config-validate); [`node_init` (node.c)](#fn-node-c-node-init).

**Chamadores diretos encontrados nos fontes inventariados:** [`initialize_local_identity` (superpeer_app.c)](#fn-superpeer-app-c-initialize-local-identity); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`test_superpeer_members_are_thread_safe` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOMEM`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_create, para acompanhar a explicação</summary>

```c
int superpeer_create(const SuperPeerConfig *config, SuperPeer **output)
{
    // Valida parâmetros de entrada e inicializa o nó local.
    SuperPeer *superpeer;
    size_t initial_capacity;
    int mutex_error;
    Node local_node;

    if (output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *output = NULL;
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_validate(&config->node) == -1 || node_init(&local_node, &config->node) == -1)
    {
        return -1;
    }
    initial_capacity = config->initial_member_capacity == 0U ? SUPERPEER_DEFAULT_MEMBER_CAPACITY : config->initial_member_capacity;

    if (initial_capacity > SIZE_MAX / sizeof(SuperPeerMember))
    {
        errno = ENOMEM;
        return -1;
    }

    superpeer = calloc(1U, sizeof(*superpeer));
    if (superpeer == NULL)
    {
        return -1;
    }

    superpeer->members = calloc(initial_capacity, sizeof(*superpeer->members));
    if (superpeer->members == NULL)
    {
        free(superpeer);
        return -1;
    }

    mutex_error = pthread_mutex_init(&superpeer->members_mutex, NULL);
    if (mutex_error != 0)
    {
        free(superpeer->members);
        free(superpeer);
        errno = mutex_error;
        return -1;
    }

    // Inicializa o nó local com papel de Super Peer e registra-o como o primeiro membro.
    local_node.role = NODE_ROLE_SUPERPEER;
    superpeer->local_node = local_node;
    superpeer->member_capacity = initial_capacity;
    set_member(&superpeer->members[0], &superpeer->local_node);
    superpeer->member_count = 1U;
    *output = superpeer;
    return 0;
}
```

</details>

<a id="fn-membership-c-superpeer-destroy"></a>

#### superpeer_destroy

Fonte: `membership.c:223` (linha nesta revisão; pode mudar em futuras edições).

```c
void superpeer_destroy(SuperPeer *superpeer);
```

destrói mutex, vetor e objeto. Só chame depois de encerrar threads que ainda consultam membership.

**Parâmetros**

- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`test_superpeer_members_are_thread_safe` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de superpeer_destroy, para acompanhar a explicação</summary>

```c
void superpeer_destroy(SuperPeer *superpeer)
{
    if (superpeer == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&superpeer->members_mutex);
    free(superpeer->members);
    free(superpeer);
}
```

</details>

<a id="fn-membership-c-superpeer-get-node"></a>

#### superpeer_get_node

Fonte: `membership.c:236` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_get_node(const SuperPeer *superpeer, Node *output);
```

copia o nó local para o chamador; não expõe ponteiro interno.

**Parâmetros**

- `const SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `Node *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Chamadores diretos encontrados nos fontes inventariados:** [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_get_node, para acompanhar a explicação</summary>

```c
int superpeer_get_node(const SuperPeer *superpeer, Node *output)
{
    if (superpeer == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *output = superpeer->local_node;
    return 0;
}
```

</details>

<a id="fn-membership-c-superpeer-register-node"></a>

#### superpeer_register_node

Fonte: `membership.c:249` (linha nesta revisão; pode mudar em futuras edições).

```c
SuperPeerRegistrationResult superpeer_register_node(SuperPeer *superpeer, const Node *node);
```

valida consistência do Node, busca por NodeID sob mutex e atualiza o membro existente ou acrescenta novo. Retorna ADDED, UPDATED ou REGISTER_ERROR; repetição do mesmo ID não aumenta contagem.

**Parâmetros**

- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `const Node *node`: Estrutura que reúne identidade, configuração, PID e papel de um nó.

**Funções do projeto usadas diretamente:** [`lock_members` (membership.c)](#fn-membership-c-lock-members); [`unlock_members` (membership.c)](#fn-membership-c-unlock-members); [`find_member_index_locked` (membership.c)](#fn-membership-c-find-member-index-locked); [`grow_members_locked` (membership.c)](#fn-membership-c-grow-members-locked); [`set_member` (membership.c)](#fn-membership-c-set-member); [`node_validate` (node.c)](#fn-node-c-node-validate).

**Chamadores diretos encontrados nos fontes inventariados:** [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`register_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-register-members).

**Retorno:** as expressões presentes nesta função são `SUPERPEER_REGISTER_ERROR`, `(SuperPeerRegistrationResult)result`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_register_node, para acompanhar a explicação</summary>

```c
SuperPeerRegistrationResult superpeer_register_node(SuperPeer *superpeer, const Node *node)
{
    size_t index;
    int result;

    if (superpeer == NULL || node == NULL)
    {
        errno = EINVAL;
        return SUPERPEER_REGISTER_ERROR;
    }
    if (node_validate(node) == -1)
    {
        return SUPERPEER_REGISTER_ERROR;
    }

    if (lock_members(superpeer) == -1)
    {
        return SUPERPEER_REGISTER_ERROR;
    }

    if (find_member_index_locked(superpeer, &node->id, &index) == 0)
    {
        set_member(&superpeer->members[index], node);
        result = SUPERPEER_MEMBER_UPDATED;
    }
    else if (grow_members_locked(superpeer) == -1)
    {
        result = SUPERPEER_REGISTER_ERROR;
    }
    else
    {
        set_member(&superpeer->members[superpeer->member_count], node);
        ++superpeer->member_count;
        result = SUPERPEER_MEMBER_ADDED;
    }

    if (unlock_members(superpeer) == -1)
    {
        return SUPERPEER_REGISTER_ERROR;
    }
    return (SuperPeerRegistrationResult)result;
}
```

</details>

<a id="fn-membership-c-superpeer-unregister-node"></a>

#### superpeer_unregister_node

Fonte: `membership.c:293` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id);
```

remove membro não local, compactando o vetor com memmove. Rejeita a remoção do próprio Super Peer. O handler de LEAVE identificado usa a remoção de membro e retira suas disponibilidades do índice antes do ACK; o comando legado anônimo de LEAVE não demonstra esse fluxo completo.

**Parâmetros**

- `SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `const NodeID *node_id`: Identificador binário de nó, com 32 bytes.

**Funções do projeto usadas diretamente:** [`lock_members` (membership.c)](#fn-membership-c-lock-members); [`unlock_members` (membership.c)](#fn-membership-c-unlock-members); [`find_member_index_locked` (membership.c)](#fn-membership-c-find-member-index-locked); [`node_id_equal` (node.c)](#fn-node-c-node-id-equal).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EPERM`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_unregister_node, para acompanhar a explicação</summary>

```c
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id)
{
    size_t index;
    size_t remaining;

    if (superpeer == NULL || node_id == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_id_equal(&superpeer->local_node.id, node_id))
    {
        errno = EPERM;
        return -1;
    }
    if (lock_members(superpeer) == -1)
    {
        return -1;
    }

    if (find_member_index_locked(superpeer, node_id, &index) == -1)
    {
        unlock_members(superpeer);
        errno = ENOENT;
        return -1;
    }

    remaining = superpeer->member_count - index - 1U;
    if (remaining > 0U)
    {
        memmove(&superpeer->members[index], &superpeer->members[index + 1U], remaining * sizeof(*superpeer->members));
    }
    --superpeer->member_count;

    if (unlock_members(superpeer) == -1)
    {
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-membership-c-superpeer-find-member"></a>

#### superpeer_find_member

Fonte: `membership.c:335` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output);
```

busca sob mutex e entrega uma **cópia** do membro. Evita deixar o cliente com ponteiro invalidado por realloc.

**Parâmetros**

- `const SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `const NodeID *node_id`: Identificador binário de nó, com 32 bytes.
- `SuperPeerMember *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`lock_members` (membership.c)](#fn-membership-c-lock-members); [`unlock_members` (membership.c)](#fn-membership-c-unlock-members); [`find_member_index_locked` (membership.c)](#fn-membership-c-find-member-index-locked).

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_find_member, para acompanhar a explicação</summary>

```c
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output)
{
    SuperPeer *mutable_superpeer;
    size_t index;

    if (superpeer == NULL || node_id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return -1;
    }

    if (find_member_index_locked(superpeer, node_id, &index) == -1)
    {
        unlock_members(mutable_superpeer);
        errno = ENOENT;
        return -1;
    }

    *output = superpeer->members[index];
    if (unlock_members(mutable_superpeer) == -1)
    {
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-membership-c-superpeer-member-count"></a>

#### superpeer_member_count

Fonte: `membership.c:368` (linha nesta revisão; pode mudar em futuras edições).

```c
size_t superpeer_member_count(const SuperPeer *superpeer);
```

lê a quantidade sob mutex, incluindo o nó local. Zero pode significar erro, além de contagem zero.

**Parâmetros**

- `const SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.

**Funções do projeto usadas diretamente:** [`lock_members` (membership.c)](#fn-membership-c-lock-members); [`unlock_members` (membership.c)](#fn-membership-c-unlock-members).

**Chamadores diretos encontrados nos fontes inventariados:** [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`test_superpeer_members_are_thread_safe` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe).

**Retorno:** as expressões presentes nesta função são `0U`, `count`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_member_count, para acompanhar a explicação</summary>

```c
size_t superpeer_member_count(const SuperPeer *superpeer)
{
    SuperPeer *mutable_superpeer;
    size_t count;

    if (superpeer == NULL)
    {
        errno = EINVAL;
        return 0U;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return 0U;
    }
    count = superpeer->member_count;
    unlock_members(mutable_superpeer);
    return count;
}
```

</details>

<a id="fn-membership-c-superpeer-is-registered"></a>

#### superpeer_is_registered

Fonte: `membership.c:390` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id);
```

consulta existência sob mutex. Usada antes de aceitar ANNOUNCE. Zero representa ausência ou falha, distinguível por contexto/errno.

**Parâmetros**

- `const SuperPeer *superpeer`: Instância de membership; em app_config_load, o int seleciona padrões do papel Super Peer.
- `const NodeID *node_id`: Identificador binário de nó, com 32 bytes.

**Funções do projeto usadas diretamente:** [`lock_members` (membership.c)](#fn-membership-c-lock-members); [`unlock_members` (membership.c)](#fn-membership-c-unlock-members); [`find_member_index_locked` (membership.c)](#fn-membership-c-find-member-index-locked).

**Chamadores diretos encontrados nos fontes inventariados:** [`register_announcement` (superpeer_app.c)](#fn-superpeer-app-c-register-announcement); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters).

**Retorno:** as expressões presentes nesta função são `0`, `registered`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de superpeer_is_registered, para acompanhar a explicação</summary>

```c
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id)
{
    SuperPeer *mutable_superpeer;
    int registered;

    if (superpeer == NULL || node_id == NULL)
    {
        errno = EINVAL;
        return 0;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return 0;
    }
    registered = find_member_index_locked(superpeer, node_id, NULL) == 0;
    unlock_members(mutable_superpeer);
    return registered;
}
```

</details>

<a id="mod-metadata-c"></a>

### metadata.c

[fail](#fn-metadata-c-fail) · [object_id_file](#fn-metadata-c-object-id-file) · [object_id_to_hex](#fn-metadata-c-object-id-to-hex) · [bucket](#fn-metadata-c-bucket) · [lock_store](#fn-metadata-c-lock-store) · [find_entry](#fn-metadata-c-find-entry) · [free_entry](#fn-metadata-c-free-entry) · [metadata_create](#fn-metadata-c-metadata-create) · [metadata_destroy](#fn-metadata-c-metadata-destroy) · [metadata_register_document](#fn-metadata-c-metadata-register-document) · [metadata_find_document](#fn-metadata-c-metadata-find-document) · [metadata_remove_document](#fn-metadata-c-metadata-remove-document) · [change_chunk](#fn-metadata-c-change-chunk) · [metadata_register_chunk](#fn-metadata-c-metadata-register-chunk) · [metadata_unregister_chunk](#fn-metadata-c-metadata-unregister-chunk) · [metadata_chunk_peers](#fn-metadata-c-metadata-chunk-peers) · [metadata_announce](#fn-metadata-c-metadata-announce) · [metadata_find_name](#fn-metadata-c-metadata-find-name) · [metadata_chunk_descriptor](#fn-metadata-c-metadata-chunk-descriptor) · [metadata_remove_peer](#fn-metadata-c-metadata-remove-peer)

<a id="fn-metadata-c-fail"></a>

#### fail

Fonte: `metadata.c:32` (linha nesta revisão; pode mudar em futuras edições).

```c
static int fail(int error);
```

Define errno com o código recebido e devolve -1. É um atalho local de tratamento de erro; não imprime mensagem, não libera recursos e não encerra o processo.

**Parâmetros**

- `int error`: Código de erro a registrar ou codificar.

**Chamadores diretos encontrados nos fontes inventariados:** [`object_id_file` (metadata.c)](#fn-metadata-c-object-id-file); [`object_id_to_hex` (metadata.c)](#fn-metadata-c-object-id-to-hex); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`metadata_create` (metadata.c)](#fn-metadata-c-metadata-create); [`metadata_register_document` (metadata.c)](#fn-metadata-c-metadata-register-document); [`metadata_find_document` (metadata.c)](#fn-metadata-c-metadata-find-document); [`metadata_remove_document` (metadata.c)](#fn-metadata-c-metadata-remove-document); [`change_chunk` (metadata.c)](#fn-metadata-c-change-chunk); [`metadata_chunk_peers` (metadata.c)](#fn-metadata-c-metadata-chunk-peers); [`metadata_announce` (metadata.c)](#fn-metadata-c-metadata-announce); [`metadata_find_name` (metadata.c)](#fn-metadata-c-metadata-find-name); [`metadata_chunk_descriptor` (metadata.c)](#fn-metadata-c-metadata-chunk-descriptor); [`metadata_remove_peer` (metadata.c)](#fn-metadata-c-metadata-remove-peer).

**Retorno:** as expressões presentes nesta função são `-1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de fail, para acompanhar a explicação</summary>

```c
static int fail(int error)
{
    errno = error;
    return -1;
}
```

</details>

<a id="fn-metadata-c-object-id-file"></a>

#### object_id_file

Fonte: `metadata.c:38` (linha nesta revisão; pode mudar em futuras edições).

```c
int object_id_file(const char *path, ObjectID *output, uint64_t *file_size);
```

lê o arquivo em blocos de 64 KiB, alimenta SHA-256 incremental da libcrypto, soma tamanho com verificação de overflow e devolve digest/tamanho. Não carrega o PDF inteiro em memória; o chamador deve evitar alterações concorrentes no arquivo durante o cálculo.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.
- `ObjectID *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.
- `uint64_t *file_size`: Tamanho original em bytes ou ponteiro de saída para esse tamanho.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `EVP_DigestUpdate`: Acrescenta bytes ao SHA-256 incremental, mantendo apenas o estado do hash e o bloco corrente.
- `EVP_DigestFinal_ex`: Finaliza o digest e grava seus bytes; o código confere o tamanho esperado.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `fail(ENOMEM)`, `fail(error)`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOMEM`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de object_id_file, para acompanhar a explicação</summary>

```c
int object_id_file(const char *path, ObjectID *output, uint64_t *file_size)
{
    unsigned char buffer[65536];
    ObjectID result;
    uint64_t total = 0;
    unsigned int length = 0;
    int error = 0;
    if (path == NULL || output == NULL || file_size == NULL) return fail(EINVAL);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) { fclose(file); return fail(ENOMEM); }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) error = EIO;
    while (error == 0)
    {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n > UINT64_MAX - total) { error = EOVERFLOW; break; }
        total += n;
        if (n != 0 && EVP_DigestUpdate(ctx, buffer, n) != 1) { error = EIO; break; }
        if (n < sizeof(buffer)) { if (ferror(file)) error = EIO; break; }
    }
    if (error == 0 && (EVP_DigestFinal_ex(ctx, result.bytes, &length) != 1 || length != OBJECT_ID_SIZE)) error = EIO;
    EVP_MD_CTX_free(ctx);
    if (fclose(file) != 0 && error == 0) error = errno;
    if (error != 0) return fail(error);
    *output = result;
    *file_size = total;
    return 0;
}
```

</details>

<a id="fn-metadata-c-object-id-to-hex"></a>

#### object_id_to_hex

Fonte: `metadata.c:68` (linha nesta revisão; pode mudar em futuras edições).

```c
int object_id_to_hex(const ObjectID *id, char *output, size_t capacity);
```

escreve 64 dígitos hexadecimais e terminador.

**Parâmetros**

- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `char *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.
- `size_t capacity`: Capacidade disponível para escrita ou limite de busca; não indica conteúdo já preenchido.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`object_hex` (storage.c)](#fn-storage-c-object-hex); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`cleanup_storage` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-cleanup-storage).

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de object_id_to_hex, para acompanhar a explicação</summary>

```c
int object_id_to_hex(const ObjectID *id, char *output, size_t capacity)
{
    const char hex[] = "0123456789abcdef";
    if (id == NULL || output == NULL || capacity < OBJECT_ID_HEX_SIZE) return fail(EINVAL);
    for (size_t i = 0; i < OBJECT_ID_SIZE; ++i) { output[i * 2] = hex[id->bytes[i] >> 4]; output[i * 2 + 1] = hex[id->bytes[i] & 15]; }
    output[64] = '\0';
    return 0;
}
```

</details>

<a id="fn-metadata-c-bucket"></a>

#### bucket

Fonte: `metadata.c:78` (linha nesta revisão; pode mudar em futuras edições).

```c
static size_t bucket(const ObjectID *id);
```

calcula índice de bucket a partir de todos os bytes do ObjectID. Possíveis colisões são resolvidas depois pela comparação completa do ID.

**Parâmetros**

- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Chamadores diretos encontrados nos fontes inventariados:** [`find_entry` (metadata.c)](#fn-metadata-c-find-entry).

**Retorno:** as expressões presentes nesta função são `value`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de bucket, para acompanhar a explicação</summary>

```c
static size_t bucket(const ObjectID *id)
{
    size_t value = 0;
    for (size_t i = 0; i < OBJECT_ID_SIZE; ++i) value = (value * 33U + id->bytes[i]) % BUCKET_COUNT;
    return value;
}
```

</details>

<a id="fn-metadata-c-lock-store"></a>

#### lock_store

Fonte: `metadata.c:85` (linha nesta revisão; pode mudar em futuras edições).

```c
static int lock_store(MetadataStore *store);
```

adquire o mutex e traduz eventual código de pthread para errno.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail).

**Chamadores diretos encontrados nos fontes inventariados:** [`metadata_register_document` (metadata.c)](#fn-metadata-c-metadata-register-document); [`metadata_find_document` (metadata.c)](#fn-metadata-c-metadata-find-document); [`metadata_remove_document` (metadata.c)](#fn-metadata-c-metadata-remove-document); [`change_chunk` (metadata.c)](#fn-metadata-c-change-chunk); [`metadata_chunk_peers` (metadata.c)](#fn-metadata-c-metadata-chunk-peers); [`metadata_announce` (metadata.c)](#fn-metadata-c-metadata-announce); [`metadata_find_name` (metadata.c)](#fn-metadata-c-metadata-find-name); [`metadata_chunk_descriptor` (metadata.c)](#fn-metadata-c-metadata-chunk-descriptor); [`metadata_remove_peer` (metadata.c)](#fn-metadata-c-metadata-remove-peer).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.

**Retorno:** as expressões presentes nesta função são `error == 0 ? 0 : fail(error)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de lock_store, para acompanhar a explicação</summary>

```c
static int lock_store(MetadataStore *store)
{
    int error = pthread_mutex_lock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}
```

</details>

<a id="fn-metadata-c-find-entry"></a>

#### find_entry

Fonte: `metadata.c:92` (linha nesta revisão; pode mudar em futuras edições).

```c
static Entry **find_entry(MetadataStore *store, const ObjectID *id);
```

percorre a lista no bucket e devolve o endereço do ponteiro da entrada; simplifica inserção/remoção. Exige mutex adquirido.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Funções do projeto usadas diretamente:** [`bucket` (metadata.c)](#fn-metadata-c-bucket).

**Chamadores diretos encontrados nos fontes inventariados:** [`metadata_register_document` (metadata.c)](#fn-metadata-c-metadata-register-document); [`metadata_find_document` (metadata.c)](#fn-metadata-c-metadata-find-document); [`metadata_remove_document` (metadata.c)](#fn-metadata-c-metadata-remove-document); [`change_chunk` (metadata.c)](#fn-metadata-c-change-chunk); [`metadata_chunk_peers` (metadata.c)](#fn-metadata-c-metadata-chunk-peers); [`metadata_announce` (metadata.c)](#fn-metadata-c-metadata-announce); [`metadata_chunk_descriptor` (metadata.c)](#fn-metadata-c-metadata-chunk-descriptor).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `entry`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de find_entry, para acompanhar a explicação</summary>

```c
static Entry **find_entry(MetadataStore *store, const ObjectID *id)
{
    Entry **entry = &store->buckets[bucket(id)];
    while (*entry != NULL && memcmp((*entry)->document.id.bytes, id->bytes, OBJECT_ID_SIZE) != 0) entry = &(*entry)->next;
    return entry;
}
```

</details>

<a id="fn-metadata-c-free-entry"></a>

#### free_entry

Fonte: `metadata.c:99` (linha nesta revisão; pode mudar em futuras edições).

```c
static void free_entry(Entry *entry);
```

Percorre e libera a lista encadeada de disponibilidades, libera o vetor de descritores e a entrada. Não deve ser chamada enquanto outro usuário puder acessar essa entrada. É a base da limpeza de candidatos em falha, remoção e destruição do índice.

**Parâmetros**

- `Entry *entry`: Entrada interna da hash table; não deve escapar como ponteiro de uso concorrente sem proteção.

**Chamadores diretos encontrados nos fontes inventariados:** [`metadata_destroy` (metadata.c)](#fn-metadata-c-metadata-destroy); [`metadata_remove_document` (metadata.c)](#fn-metadata-c-metadata-remove-document); [`metadata_announce` (metadata.c)](#fn-metadata-c-metadata-announce).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de free_entry, para acompanhar a explicação</summary>

```c
static void free_entry(Entry *entry)
{
    while (entry->available != NULL) { Availability *next = entry->available->next; free(entry->available); entry->available = next; }
    free(entry->chunks);
    free(entry);
}
```

</details>

<a id="fn-metadata-c-metadata-create"></a>

#### metadata_create

Fonte: `metadata.c:106` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_create(MetadataStore **output);
```

aloca store zerado, inicializa mutex e devolve ponteiro opaco.

**Parâmetros**

- `MetadataStore **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `fail(error)`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_create, para acompanhar a explicação</summary>

```c
int metadata_create(MetadataStore **output)
{
    if (output == NULL) return fail(EINVAL);
    *output = NULL;
    MetadataStore *store = calloc(1, sizeof(*store));
    if (store == NULL) return -1;
    int error = pthread_mutex_init(&store->mutex, NULL);
    if (error != 0) { free(store); return fail(error); }
    *output = store;
    return 0;
}
```

</details>

<a id="fn-metadata-c-metadata-destroy"></a>

#### metadata_destroy

Fonte: `metadata.c:118` (linha nesta revisão; pode mudar em futuras edições).

```c
void metadata_destroy(MetadataStore *store);
```

libera todas as entradas e o mutex; exige que outras threads já tenham parado de usá-lo.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.

**Funções do projeto usadas diretamente:** [`free_entry` (metadata.c)](#fn-metadata-c-free-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de metadata_destroy, para acompanhar a explicação</summary>

```c
void metadata_destroy(MetadataStore *store)
{
    if (store == NULL) return;
    for (size_t i = 0; i < BUCKET_COUNT; ++i) { Entry *entry = store->buckets[i]; while (entry != NULL) { Entry *next = entry->next; free_entry(entry); entry = next; } }
    pthread_mutex_destroy(&store->mutex);
    free(store);
}
```

</details>

<a id="fn-metadata-c-metadata-register-document"></a>

#### metadata_register_document

Fonte: `metadata.c:126` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_register_document(MetadataStore *store, const ObjectID *id, const char *name, uint64_t file_size);
```

cria documento e calcula chunk_count, ou aceita repetição do mesmo ID/tamanho. Se outro nome vier para o mesmo ID, o primeiro nome fica preservado; tamanho diferente gera EEXIST.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `const char *name`: Nome textual usado como chave, metadado ou variável de ambiente, conforme a finalidade descrita.
- `uint64_t file_size`: Tamanho original em bytes ou ponteiro de saída para esse tamanho.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `error == 0 ? 0 : fail(error)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_register_document, para acompanhar a explicação</summary>

```c
int metadata_register_document(MetadataStore *store, const ObjectID *id, const char *name, uint64_t file_size)
{
    if (store == NULL || id == NULL || name == NULL || name[0] == '\0' || strnlen(name, METADATA_NAME_SIZE) == METADATA_NAME_SIZE) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry **slot = find_entry(store, id);
    int error = 0;
    if (*slot != NULL) { if ((*slot)->document.file_size != file_size) error = EEXIST; }
    else
    {
        Entry *entry = calloc(1, sizeof(*entry));
        if (entry == NULL) error = ENOMEM;
        else
        {
            entry->document.id = *id;
            strcpy(entry->document.name, name);
            entry->document.file_size = file_size;
            entry->document.chunk_count = file_size / METADATA_CHUNK_SIZE + (file_size % METADATA_CHUNK_SIZE != 0);
            *slot = entry;
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}
```

</details>

<a id="fn-metadata-c-metadata-find-document"></a>

#### metadata_find_document

Fonte: `metadata.c:150` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_find_document(MetadataStore *store, const ObjectID *id, MetadataDocument *output);
```

procura pelo ObjectID e devolve cópia de MetadataDocument. ENOENT quando não existe.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `MetadataDocument *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `found ? 0 : fail(ENOENT)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_find_document, para acompanhar a explicação</summary>

```c
int metadata_find_document(MetadataStore *store, const ObjectID *id, MetadataDocument *output)
{
    if (store == NULL || id == NULL || output == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry *entry = *find_entry(store, id);
    int found = entry != NULL;
    if (found) *output = entry->document;
    pthread_mutex_unlock(&store->mutex);
    return found ? 0 : fail(ENOENT);
}
```

</details>

<a id="fn-metadata-c-metadata-remove-document"></a>

#### metadata_remove_document

Fonte: `metadata.c:161` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_remove_document(MetadataStore *store, const ObjectID *id);
```

remove documento e todas as suas associações de chunk; é uma operação da API local, não um fluxo de exclusão de documento distribuído.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry); [`free_entry` (metadata.c)](#fn-metadata-c-free-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `found ? 0 : fail(ENOENT)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_remove_document, para acompanhar a explicação</summary>

```c
int metadata_remove_document(MetadataStore *store, const ObjectID *id)
{
    if (store == NULL || id == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry **slot = find_entry(store, id);
    Entry *entry = *slot;
    int found = entry != NULL;
    if (found) { *slot = entry->next; free_entry(entry); }
    pthread_mutex_unlock(&store->mutex);
    return found ? 0 : fail(ENOENT);
}
```

</details>

<a id="fn-metadata-c-change-chunk"></a>

#### change_chunk

Fonte: `metadata.c:174` (linha nesta revisão; pode mudar em futuras edições).

```c
static int change_chunk(MetadataStore *store, const ObjectID *id, uint64_t index, const NodeID *peer, int remove);
```

auxiliar comum que valida documento/índice, busca a associação específica e insere ou remove sob o mesmo mutex. Inserção repetida é idempotente; remoção ausente gera ENOENT.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.
- `const NodeID *peer`: NodeID da localização do chunk.
- `int remove`: Seletor de inserção/remoção da associação de disponibilidade.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`metadata_register_chunk` (metadata.c)](#fn-metadata-c-metadata-register-chunk); [`metadata_unregister_chunk` (metadata.c)](#fn-metadata-c-metadata-unregister-chunk).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `error == 0 ? 0 : fail(error)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de change_chunk, para acompanhar a explicação</summary>

```c
static int change_chunk(MetadataStore *store, const ObjectID *id, uint64_t index, const NodeID *peer, int remove)
{
    if (store == NULL || id == NULL || peer == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry *entry = *find_entry(store, id);
    int error = 0;
    if (entry == NULL) error = ENOENT;
    else if (index >= entry->document.chunk_count) error = EINVAL;
    else
    {
        Availability **slot = &entry->available;
        while (*slot != NULL && ((*slot)->index != index || memcmp((*slot)->peer.bytes, peer->bytes, NODE_ID_SIZE) != 0)) slot = &(*slot)->next;
        if (remove)
        {
            if (*slot == NULL) error = ENOENT;
            else { Availability *old = *slot; *slot = old->next; free(old); }
        }
        else if (*slot == NULL)
        {
            Availability *item = calloc(1, sizeof(*item));
            if (item == NULL) error = ENOMEM;
            else { item->index = index; item->peer = *peer; *slot = item; }
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}
```

</details>

<a id="fn-metadata-c-metadata-register-chunk"></a>

#### metadata_register_chunk

Fonte: `metadata.c:202` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_register_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer);
```

Delega a change_chunk no modo de inserção. Valida/insere uma associação índice–NodeID de modo idempotente, mas não verifica cadastro do nó em membership e não recebe bytes do documento.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t chunk_index`: Índice do chunk no documento, iniciado em zero.
- `const NodeID *peer`: NodeID da localização do chunk.

**Funções do projeto usadas diretamente:** [`change_chunk` (metadata.c)](#fn-metadata-c-change-chunk).

**Chamadores diretos encontrados nos fontes inventariados:** [`register_peer` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-register-peer); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Retorno:** as expressões presentes nesta função são `change_chunk(store, id, chunk_index, peer, 0)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de metadata_register_chunk, para acompanhar a explicação</summary>

```c
int metadata_register_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer)
{
    return change_chunk(store, id, chunk_index, peer, 0);
}
```

</details>

<a id="fn-metadata-c-metadata-unregister-chunk"></a>

#### metadata_unregister_chunk

Fonte: `metadata.c:207` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_unregister_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer);
```

Delega a change_chunk no modo de remoção. Remove uma localização específica de chunk, não o arquivo físico nem todo o documento; ausência da associação é erro.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t chunk_index`: Índice do chunk no documento, iniciado em zero.
- `const NodeID *peer`: NodeID da localização do chunk.

**Funções do projeto usadas diretamente:** [`change_chunk` (metadata.c)](#fn-metadata-c-change-chunk).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Retorno:** as expressões presentes nesta função são `change_chunk(store, id, chunk_index, peer, 1)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de metadata_unregister_chunk, para acompanhar a explicação</summary>

```c
int metadata_unregister_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer)
{
    return change_chunk(store, id, chunk_index, peer, 1);
}
```

</details>

<a id="fn-metadata-c-metadata-chunk-peers"></a>

#### metadata_chunk_peers

Fonte: `metadata.c:212` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_chunk_peers(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, NodeID **output, size_t *count);
```

conta e copia todos os NodeIDs que anunciam aquele chunk. O chamador libera a matriz retornada com free; sem localizações, devolve NULL e contagem zero.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t chunk_index`: Índice do chunk no documento, iniciado em zero.
- `NodeID **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `size_t *count`: Quantidade de elementos ou ponteiro para devolvê-la; não necessariamente bytes.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `error == 0 ? 0 : fail(error)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_chunk_peers, para acompanhar a explicação</summary>

```c
int metadata_chunk_peers(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, NodeID **output, size_t *count)
{
    if (store == NULL || id == NULL || output == NULL || count == NULL) return fail(EINVAL);
    if (lock_store(store) != 0) return -1;
    Entry *entry = *find_entry(store, id);
    int error = 0;
    size_t n = 0;
    NodeID *peers = NULL;
    if (entry == NULL) error = ENOENT;
    else if (chunk_index >= entry->document.chunk_count) error = EINVAL;
    else
    {
        for (Availability *item = entry->available; item != NULL; item = item->next) if (item->index == chunk_index) ++n;
        if (n > SIZE_MAX / sizeof(*peers)) error = EOVERFLOW;
        else if (n != 0 && (peers = malloc(n * sizeof(*peers))) == NULL) error = ENOMEM;
        if (error == 0)
        {
            size_t i = 0;
            for (Availability *item = entry->available; item != NULL; item = item->next) if (item->index == chunk_index) peers[i++] = item->peer;
            *output = peers;
            *count = n;
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return error == 0 ? 0 : fail(error);
}
```

</details>

<a id="fn-metadata-c-metadata-announce"></a>

#### metadata_announce

Fonte: `metadata.c:240` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_announce(MetadataStore *store, const MetadataDocument *document, const MetadataChunk *chunks, const NodeID *owner);
```

valida descritores e cria candidato completo antes de lock/publicação. Dentro da seção crítica confere conflitos, preserva dono/nome original, combina localizações e troca a entrada apenas no sucesso.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const MetadataDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `const MetadataChunk *chunks`: Vetor de descritores/localizações; quantidade vem do documento associado.
- `const NodeID *owner`: NodeID do nó proprietário/anunciante, não um PID.

**Passo a passo**

1. Valida documento e quantidade esperada de chunks; prepara entrada candidata e descritores sem publicar parcialmente.
2. Confere índice, offset e tamanhos de cada descritor e cria associações com o owner anunciante.
3. Sob mutex do índice, encontra uma entrada antiga e verifica conflitos com seu tamanho/descritores.
4. Preserva os metadados de identidade do registro anterior e combina suas localizações, sem repetir o mesmo owner.
5. Troca o ponteiro da entrada apenas após a preparação bem-sucedida. Na falha, libera o candidato e mantém a entrada antiga.
6. O Super Peer verifica coerência do anúncio; não recalcula SHA sobre PDF, porque não possui os bytes.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry); [`free_entry` (metadata.c)](#fn-metadata-c-free-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_announce` (directory.c)](#fn-directory-c-directory-announce); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `fail(error)`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_announce, para acompanhar a explicação</summary>

```c
int metadata_announce(MetadataStore *store, const MetadataDocument *document, const MetadataChunk *chunks, const NodeID *owner)
{
    if (store == NULL || document == NULL || chunks == NULL || owner == NULL || document->chunk_count == 0U || document->chunk_count > SIZE_MAX / sizeof(*chunks) || document->chunk_count != document->file_size / METADATA_CHUNK_SIZE + (document->file_size % METADATA_CHUNK_SIZE != 0U) || strnlen(document->name, METADATA_NAME_SIZE) == METADATA_NAME_SIZE) return fail(EINVAL);
    Entry *candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) return -1;
    candidate->document = *document;
    candidate->document.owner = *owner;
    candidate->document.version = 1U;
    candidate->chunks = malloc((size_t)document->chunk_count * sizeof(*chunks));
    if (candidate->chunks == NULL) { free_entry(candidate); return -1; }
    memcpy(candidate->chunks, chunks, (size_t)document->chunk_count * sizeof(*chunks));
    for (uint64_t i = 0U; i < document->chunk_count; ++i)
    {
        uint64_t offset = i * METADATA_CHUNK_SIZE;
        uint64_t remaining = document->file_size - offset;
        if (chunks[i].index != i || chunks[i].offset != offset || chunks[i].raw_size != (remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining) || chunks[i].compressed_size == 0U) { free_entry(candidate); return fail(EINVAL); }
        Availability *item = calloc(1U, sizeof(*item));
        if (item == NULL) { free_entry(candidate); return -1; }
        item->index = i;
        item->peer = *owner;
        item->next = candidate->available;
        candidate->available = item;
    }
    if (lock_store(store) < 0) { free_entry(candidate); return -1; }
    Entry **slot = find_entry(store, &document->id);
    Entry *old = *slot;
    int error = 0;
    if (old != NULL)
    {
        if (old->document.file_size != document->file_size || old->document.chunk_count != document->chunk_count) error = EEXIST;
        for (uint64_t i = 0U; error == 0 && old->chunks != NULL && i < document->chunk_count; ++i)
            if (old->chunks[i].raw_size != chunks[i].raw_size || memcmp(old->chunks[i].hash, chunks[i].hash, OBJECT_ID_SIZE) != 0) error = EEXIST;
        for (Availability *item = old->available; error == 0 && item != NULL; item = item->next)
        {
            if (memcmp(item->peer.bytes, owner->bytes, NODE_ID_SIZE) == 0) continue;
            Availability *copy = malloc(sizeof(*copy));
            if (copy == NULL) { error = ENOMEM; break; }
            *copy = *item;
            copy->next = candidate->available;
            candidate->available = copy;
        }
        if (error == 0) { candidate->document = old->document; candidate->next = old->next; }
    }
    if (error == 0) { *slot = candidate; if (old != NULL) free_entry(old); }
    pthread_mutex_unlock(&store->mutex);
    if (error != 0) { free_entry(candidate); return fail(error); }
    return 0;
}
```

</details>

<a id="fn-metadata-c-metadata-find-name"></a>

#### metadata_find_name

Fonte: `metadata.c:289` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_find_name(MetadataStore *store, const char *name, ObjectID *id);
```

percorre os buckets sob mutex; devolve ObjectID único, ENOENT ou ENOTUNIQ.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const char *name`: Nome textual usado como chave, metadado ou variável de ambiente, conforme a finalidade descrita.
- `ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store).

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `fail(ENOTUNIQ)`, `fail(ENOENT)`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOTUNIQ`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_find_name, para acompanhar a explicação</summary>

```c
int metadata_find_name(MetadataStore *store, const char *name, ObjectID *id)
{
    if (store == NULL || name == NULL || id == NULL) return fail(EINVAL);
    if (lock_store(store) < 0) return -1;
    int found = 0;
    ObjectID result = {{0}};
    for (size_t i = 0U; i < BUCKET_COUNT; ++i)
        for (Entry *entry = store->buckets[i]; entry != NULL; entry = entry->next)
            if (strcmp(entry->document.name, name) == 0)
            {
                if (found) { pthread_mutex_unlock(&store->mutex); return fail(ENOTUNIQ); }
                result = entry->document.id;
                found = 1;
            }
    pthread_mutex_unlock(&store->mutex);
    if (!found) return fail(ENOENT);
    *id = result;
    return 0;
}
```

</details>

<a id="fn-metadata-c-metadata-chunk-descriptor"></a>

#### metadata_chunk_descriptor

Fonte: `metadata.c:309` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_chunk_descriptor(MetadataStore *store, const ObjectID *id, uint64_t index, MetadataChunk *output);
```

devolve cópia consistente do descritor, sem ponteiros emprestados.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.
- `MetadataChunk *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store); [`find_entry` (metadata.c)](#fn-metadata-c-find-entry).

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `valid ? 0 : fail(ENOENT)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_chunk_descriptor, para acompanhar a explicação</summary>

```c
int metadata_chunk_descriptor(MetadataStore *store, const ObjectID *id, uint64_t index, MetadataChunk *output)
{
    if (store == NULL || id == NULL || output == NULL) return fail(EINVAL);
    if (lock_store(store) < 0) return -1;
    Entry *entry = *find_entry(store, id);
    int valid = entry != NULL && entry->chunks != NULL && index < entry->document.chunk_count;
    if (valid) *output = entry->chunks[index];
    pthread_mutex_unlock(&store->mutex);
    return valid ? 0 : fail(ENOENT);
}
```

</details>

<a id="fn-metadata-c-metadata-remove-peer"></a>

#### metadata_remove_peer

Fonte: `metadata.c:320` (linha nesta revisão; pode mudar em futuras edições).

```c
int metadata_remove_peer(MetadataStore *store, const NodeID *peer);
```

remove todas as associações do NodeID; preserva documentos e descritores para eventual novo anúncio.

**Parâmetros**

- `MetadataStore *store`: Instância da hash table de metadados, não bytes do PDF.
- `const NodeID *peer`: NodeID da localização do chunk.

**Funções do projeto usadas diretamente:** [`fail` (metadata.c)](#fn-metadata-c-fail); [`lock_store` (metadata.c)](#fn-metadata-c-lock-store).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`main` (tests/c2/test_metadata.c)](#fn-tests-c2-test-metadata-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `fail(EINVAL)`, `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de metadata_remove_peer, para acompanhar a explicação</summary>

```c
int metadata_remove_peer(MetadataStore *store, const NodeID *peer)
{
    if (store == NULL || peer == NULL) return fail(EINVAL);
    if (lock_store(store) < 0) return -1;
    for (size_t i = 0U; i < BUCKET_COUNT; ++i)
        for (Entry *entry = store->buckets[i]; entry != NULL; entry = entry->next)
        {
            Availability **slot = &entry->available;
            while (*slot != NULL)
            {
                if (memcmp((*slot)->peer.bytes, peer->bytes, NODE_ID_SIZE) == 0) { Availability *old = *slot; *slot = old->next; free(old); }
                else slot = &(*slot)->next;
            }
        }
    pthread_mutex_unlock(&store->mutex);
    return 0;
}
```

</details>

<a id="mod-network-c"></a>

### network.c

[timeout_ms](#fn-network-c-timeout-ms) · [now_ms](#fn-network-c-now-ms) · [wait_ready](#fn-network-c-wait-ready) · [network_create_server](#fn-network-c-network-create-server) · [network_accept_client](#fn-network-c-network-accept-client) · [network_connect](#fn-network-c-network-connect) · [network_send_all](#fn-network-c-network-send-all) · [network_recv_exact](#fn-network-c-network-recv-exact) · [network_shutdown](#fn-network-c-network-shutdown)

<a id="fn-network-c-timeout-ms"></a>

#### timeout_ms

Fonte: `network.c:14` (linha nesta revisão; pode mudar em futuras edições).

```c
static int timeout_ms(const char *name, int fallback);
```

lê timeout em segundos (1–3600) e usa padrão se inválido.

**Parâmetros**

- `const char *name`: Nome textual usado como chave, metadado ou variável de ambiente, conforme a finalidade descrita.
- `int fallback`: Timeout padrão em milissegundos caso a variável de ambiente seja inválida/ausente.

**Chamadores diretos encontrados nos fontes inventariados:** [`network_connect` (network.c)](#fn-network-c-network-connect); [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `strtol`: Converte texto para inteiro; end permite rejeitar lixo depois dos dígitos e errno permite detectar estouro.

**Retorno:** as expressões presentes nesta função são `fallback`, `errno == 0 && end != value && *end == && seconds > 0 && seconds <= 3600 ? (int)seconds * 1000 : fallback`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de timeout_ms, para acompanhar a explicação</summary>

```c
static int timeout_ms(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end;
    long seconds;
    if (value == NULL) return fallback;
    errno = 0;
    seconds = strtol(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' && seconds > 0 && seconds <= 3600 ? (int)seconds * 1000 : fallback;
}
```

</details>

<a id="fn-network-c-now-ms"></a>

#### now_ms

Fonte: `network.c:25` (linha nesta revisão; pode mudar em futuras edições).

```c
static int64_t now_ms(void);
```

lê CLOCK_MONOTONIC em milissegundos.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Chamadores diretos encontrados nos fontes inventariados:** [`wait_ready` (network.c)](#fn-network-c-wait-ready); [`network_connect` (network.c)](#fn-network-c-network-connect); [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact).

**Retorno:** as expressões presentes nesta função são `-1`, `(int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de now_ms, para acompanhar a explicação</summary>

```c
static int64_t now_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
```

</details>

<a id="fn-network-c-wait-ready"></a>

#### wait_ready

Fonte: `network.c:32` (linha nesta revisão; pode mudar em futuras edições).

```c
static int wait_ready(int fd, short events, int64_t deadline);
```

usa poll até readiness/deadline, retomando EINTR sem reiniciar o prazo.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `short events`: Eventos poll desejados, como POLLIN ou POLLOUT.
- `int64_t deadline`: Prazo absoluto em milissegundos do relógio monotônico.

**Funções do projeto usadas diretamente:** [`now_ms` (network.c)](#fn-network-c-now-ms).

**Chamadores diretos encontrados nos fontes inventariados:** [`network_connect` (network.c)](#fn-network-c-network-connect); [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `poll`: Espera prontidão em descritores por prazo limitado. Prontidão deve ser seguida da operação que confirma o resultado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ETIMEDOUT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de wait_ready, para acompanhar a explicação</summary>

```c
static int wait_ready(int fd, short events, int64_t deadline)
{
    struct pollfd descriptor = {.fd = fd, .events = events};
    for (;;)
    {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        int result = poll(&descriptor, 1U, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (result > 0) return 0;
        if (result == 0) { errno = ETIMEDOUT; return -1; }
        if (errno != EINTR) return -1;
    }
}
```

</details>

<a id="fn-network-c-network-create-server"></a>

#### network_create_server

Fonte: `network.c:46` (linha nesta revisão; pode mudar em futuras edições).

```c
int network_create_server(uint16_t porta, int backlog);
```

Cria TCP/IPv4, SO_REUSEADDR, bind no endereço configurado e listen; retorna socket de escuta ou fecha-o em erro. Backlog é a fila do kernel, não o número de workers.

**Parâmetros**

- `uint16_t porta`: Porta TCP numérica do endpoint.
- `int backlog`: Limite solicitado à fila de conexões pendentes do kernel; não é número de threads.

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `socket`: Cria um endpoint local; por si só não faz bind, listen ou connect.
- `inet_pton`: Converte endereço textual para binário; distingue 1 válido, 0 texto inválido e -1 erro da família.

**Retorno:** as expressões presentes nesta função são `-1`, `fd`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de network_create_server, para acompanhar a explicação</summary>

```c
int network_create_server(uint16_t porta, int backlog)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(porta)};
    const char *bind_ip = getenv("PEER_BIND_IP");
    int opt = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_ip != NULL && inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) { close(fd); errno = EINVAL; return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, backlog) < 0)
    {
        int error = errno; close(fd); errno = error; return -1;
    }
    return fd;
}
```

</details>

<a id="fn-network-c-network-accept-client"></a>

#### network_accept_client

Fonte: `network.c:62` (linha nesta revisão; pode mudar em futuras edições).

```c
int network_accept_client(int server_fd);
```

Aceita e devolve uma conexão; não altera o socket de escuta. O chamador fecha o descritor recebido.

**Parâmetros**

- `int server_fd`: Descritor do listener; aceitação produz outro descritor.

**Chamadores diretos encontrados nos fontes inventariados:** [`concurrent_server_run` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `accept`: Cria um descritor conectado separado do listener. Cada conexão tem vida própria.

**Retorno:** as expressões presentes nesta função são `accept(server_fd, NULL, NULL)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de network_accept_client, para acompanhar a explicação</summary>

```c
int network_accept_client(int server_fd)
{
    return accept(server_fd, NULL, NULL);
}
```

</details>

<a id="fn-network-c-network-connect"></a>

#### network_connect

Fonte: `network.c:67` (linha nesta revisão; pode mudar em futuras edições).

```c
int network_connect(const char *ip, uint16_t porta);
```

Valida IPv4 numérico e porta; conecta temporariamente em modo não bloqueante e espera POLLOUT com prazo monotônico, verifica SO_ERROR e restaura flags. Fecha o descritor preservando errno em falha.

**Parâmetros**

- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `uint16_t porta`: Porta TCP numérica do endpoint.

**Passo a passo**

1. Converte IPv4 e porta antes de abrir o socket; este caminho não resolve nomes DNS.
2. Lê as flags existentes e ativa O_NONBLOCK temporariamente.
3. connect imediato bem-sucedido dispensa espera. EINPROGRESS pede poll; outros erros encerram a tentativa.
4. Prontidão não prova sucesso de conexão: getsockopt(SO_ERROR) recupera o resultado real.
5. Restaura as flags e devolve o descritor. O rótulo failure fecha o socket mantendo o errno original.

**Funções do projeto usadas diretamente:** [`timeout_ms` (network.c)](#fn-network-c-timeout-ms); [`now_ms` (network.c)](#fn-network-c-now-ms); [`wait_ready` (network.c)](#fn-network-c-wait-ready).

**Chamadores diretos encontrados nos fontes inventariados:** [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `socket`: Cria um endpoint local; por si só não faz bind, listen ou connect.
- `inet_pton`: Converte endereço textual para binário; distingue 1 válido, 0 texto inválido e -1 erro da família.

**Retorno:** as expressões presentes nesta função são `-1`, `fd`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de network_connect, para acompanhar a explicação</summary>

```c
int network_connect(const char *ip, uint16_t porta)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(porta)};
    int fd, flags, error = 0;
    socklen_t size = sizeof(error);
    if (ip == NULL || porta == 0U || inet_pton(AF_INET, ip, &address.sin_addr) != 1) { errno = EINVAL; return -1; }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto failure;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        if (errno != EINPROGRESS || wait_ready(fd, POLLOUT, now_ms() + timeout_ms("PEER_CONNECT_TIMEOUT", 5000)) < 0) goto failure;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) goto failure;
        if (error != 0) { errno = error; goto failure; }
    }
    if (fcntl(fd, F_SETFL, flags) < 0) goto failure;
    return fd;
failure:
    error = errno; close(fd); errno = error; return -1;
}
```

</details>

<a id="fn-network-c-network-send-all"></a>

#### network_send_all

Fonte: `network.c:89` (linha nesta revisão; pode mudar em futuras edições).

```c
ssize_t network_send_all(int sock, const void *buffer, size_t size);
```

repete send até transmitir todos os bytes, pois uma chamada isolada pode enviar apenas parte deles. Trata EINTR e usa MSG_NOSIGNAL para não derrubar o processo em conexão quebrada. Devolve a quantidade total ou -1.

**Parâmetros**

- `int sock`: Descritor de socket já aberto; não contém os bytes da mensagem.
- `const void *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Passo a passo**

1. buffer é convertido para ponteiro de bytes, permitindo avançar exatamente done posições.
2. Antes do laço, rejeita comprimento que não cabe em ssize_t e ponteiro nulo com tamanho positivo.
3. send tenta somente size - done bytes. Uma quantidade positiva avança done e renova o prazo de inatividade.
4. Zero é tratado como conexão quebrada. Em -1, EINTR repete; EAGAIN/EWOULDBLOCK espera POLLOUT até o prazo; outros erros saem.
5. O retorno total só ocorre quando done == size. Não se fecha o socket nesta função: o chamador controla sua vida.

**Funções do projeto usadas diretamente:** [`timeout_ms` (network.c)](#fn-network-c-timeout-ms); [`now_ms` (network.c)](#fn-network-c-now-ms); [`wait_ready` (network.c)](#fn-network-c-wait-ready).

**Chamadores diretos encontrados nos fontes inventariados:** [`send_string` (local_control.c)](#fn-local-control-c-send-string); [`progress_write` (local_control.c)](#fn-local-control-c-progress-write); [`handle_command` (local_control.c)](#fn-local-control-c-handle-command); [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`test_protocol_rejections` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections).

**Retorno:** as expressões presentes nesta função são `-1`, `(ssize_t)done`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EPIPE`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de network_send_all, para acompanhar a explicação</summary>

```c
ssize_t network_send_all(int sock, const void *buffer, size_t size)
{
    const unsigned char *data = buffer;
    size_t done = 0U;
    int timeout = timeout_ms("PEER_IO_TIMEOUT", 30000);
    int64_t deadline = now_ms() + timeout;
    if (size > (size_t)SSIZE_MAX || (buffer == NULL && size != 0U)) { errno = EINVAL; return -1; }
    while (done < size)
    {
        ssize_t count = send(sock, data + done, size - done, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) { done += (size_t)count; deadline = now_ms() + timeout; continue; }
        if (count == 0) { errno = EPIPE; return -1; }
        if (errno == EINTR) continue;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || wait_ready(sock, POLLOUT, deadline) < 0) return -1;
    }
    return (ssize_t)done;
}
```

</details>

<a id="fn-network-c-network-recv-exact"></a>

#### network_recv_exact

Fonte: `network.c:107` (linha nesta revisão; pode mudar em futuras edições).

```c
ssize_t network_recv_exact(int sock, void *buffer, size_t size);
```

repete recv até obter exatamente tam bytes. Se o remoto fecha antes, devolve a quantidade parcial, inclusive 0 quando não recebeu nada; em erro devolve -1. O protocolo considera um header/payload parcial inválido.

**Parâmetros**

- `int sock`: Descritor de socket já aberto; não contém os bytes da mensagem.
- `void *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Passo a passo**

1. Mantém done independente do número de chamadas recv; o TCP não preserva fronteiras de mensagens.
2. Cada leitura positiva grava depois dos bytes já obtidos e renova o prazo de inatividade.
3. recv == 0 retorna done imediatamente. Pode ser zero ou um número menor que size, o que o protocolo precisa distinguir.
4. EINTR repete; EAGAIN/EWOULDBLOCK usa poll com POLLIN. Erro real ou timeout devolve -1.
5. O buffer pertence ao chamador. Em erro ou EOF parcial, seus primeiros bytes já podem ter sido alterados.

**Funções do projeto usadas diretamente:** [`timeout_ms` (network.c)](#fn-network-c-timeout-ms); [`now_ms` (network.c)](#fn-network-c-now-ms); [`wait_ready` (network.c)](#fn-network-c-wait-ready).

**Chamadores diretos encontrados nos fontes inventariados:** [`receive_string` (local_control.c)](#fn-local-control-c-receive-string); [`handle_command` (local_control.c)](#fn-local-control-c-handle-command); [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`make_wire_message` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-make-wire-message).

**Retorno:** as expressões presentes nesta função são `-1`, `(ssize_t)done`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de network_recv_exact, para acompanhar a explicação</summary>

```c
ssize_t network_recv_exact(int sock, void *buffer, size_t size)
{
    unsigned char *data = buffer;
    size_t done = 0U;
    int timeout = timeout_ms("PEER_IO_TIMEOUT", 30000);
    int64_t deadline = now_ms() + timeout;
    if (size > (size_t)SSIZE_MAX || (buffer == NULL && size != 0U)) { errno = EINVAL; return -1; }
    while (done < size)
    {
        ssize_t count = recv(sock, data + done, size - done, MSG_DONTWAIT);
        if (count > 0) { done += (size_t)count; deadline = now_ms() + timeout; continue; }
        if (count == 0) return (ssize_t)done;
        if (errno == EINTR) continue;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || wait_ready(sock, POLLIN, deadline) < 0) return -1;
    }
    return (ssize_t)done;
}
```

</details>

<a id="fn-network-c-network-shutdown"></a>

#### network_shutdown

Fonte: `network.c:125` (linha nesta revisão; pode mudar em futuras edições).

```c
int network_shutdown(int sock);
```

Executa shutdown e close; não imprime erro para sockets já desconectados. O retorno reflete close.

**Parâmetros**

- `int sock`: Descritor de socket já aberto; não contém os bytes da mensagem.

**Chamadores diretos encontrados nos fontes inventariados:** [`worker_run` (concurrent_server.c)](#fn-concurrent-server-c-worker-run); [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `shutdown`: Interrompe direções de comunicação do socket; o descritor ainda precisa de close.

**Retorno:** as expressões presentes nesta função são `close(sock)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de network_shutdown, para acompanhar a explicação</summary>

```c
int network_shutdown(int sock)
{
    (void)shutdown(sock, SHUT_RDWR);
    return close(sock);
}
```

</details>

<a id="mod-node-c"></a>

### node.c

[normalize_ip](#fn-node-c-normalize-ip) · [node_generate_uuid](#fn-node-c-node-generate-uuid) · [node_config_validate](#fn-node-c-node-config-validate) · [node_config_init_with_uuid](#fn-node-c-node-config-init-with-uuid) · [node_config_init](#fn-node-c-node-config-init) · [node_compute_id](#fn-node-c-node-compute-id) · [node_init](#fn-node-c-node-init) · [node_validate](#fn-node-c-node-validate) · [node_id_to_hex](#fn-node-c-node-id-to-hex) · [hexadecimal_value](#fn-node-c-hexadecimal-value) · [node_id_from_hex](#fn-node-c-node-id-from-hex) · [node_id_compare](#fn-node-c-node-id-compare) · [node_id_equal](#fn-node-c-node-id-equal) · [node_get_process_id](#fn-node-c-node-get-process-id)

<a id="fn-node-c-normalize-ip"></a>

#### normalize_ip

Fonte: `node.c:14` (linha nesta revisão; pode mudar em futuras edições).

```c
static int normalize_ip(const char *ip, char normalized[NODE_ADDRESS_SIZE]);
```

auxiliar privado que tenta interpretar IPv4 e depois IPv6 por inet_pton; reescreve a forma textual normalizada com inet_ntop. NodeID depende dos bytes binários, não de diferenças de grafia do endereço.

**Parâmetros**

- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `char normalized[NODE_ADDRESS_SIZE]`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.

**Chamadores diretos encontrados nos fontes inventariados:** [`node_config_validate` (node.c)](#fn-node-c-node-config-validate); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `inet_pton`: Converte endereço textual para binário; distingue 1 válido, 0 texto inválido e -1 erro da família.
- `inet_ntop`: Converte endereço binário para texto legível; não descobre identidade criptográfica.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de normalize_ip, para acompanhar a explicação</summary>

```c
static int normalize_ip(const char *ip, char normalized[NODE_ADDRESS_SIZE])
{
    struct in_addr ipv4;
    struct in6_addr ipv6;

    // Valida parâmetros de entrada.
    if (ip == NULL || normalized == NULL || ip[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    // Inicializa o buffer de saída com zeros para evitar lixo de memória.
    memset(normalized, 0, NODE_ADDRESS_SIZE);

    // Tenta interpretar o endereço como IPv4 e, se falhar, tenta como IPv6.
    if (inet_pton(AF_INET, ip, &ipv4) == 1)
    {
        if (inet_ntop(AF_INET, &ipv4, normalized, NODE_ADDRESS_SIZE) == NULL)
        {
            return -1;
        }
        return 0;
    }

    if (inet_pton(AF_INET6, ip, &ipv6) == 1)
    {
        if (inet_ntop(AF_INET6, &ipv6, normalized, NODE_ADDRESS_SIZE) == NULL)
        {
            return -1;
        }
        return 0;
    }

    errno = EINVAL;
    return -1;
}
```

</details>

<a id="fn-node-c-node-generate-uuid"></a>

#### node_generate_uuid

Fonte: `node.c:53` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_generate_uuid(uint8_t uuid[NODE_UUID_SIZE]);
```

lê 16 bytes de /dev/urandom, repetindo leituras parciais e tratando EINTR. Ajusta os bits de versão/variante de UUID v4. Um UUID novo muda o NodeID mesmo com IP e porta iguais.

**Parâmetros**

- `uint8_t uuid[NODE_UUID_SIZE]`: 16 bytes persistentes usados como componente da identidade.

**Chamadores diretos encontrados nos fontes inventariados:** [`app_identity` (app_config.c)](#fn-app-config-c-app-identity); [`node_config_init` (node.c)](#fn-node-c-node-config-init).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `read`: Pode devolver menos bytes que o solicitado, zero em EOF ou -1 em erro.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_generate_uuid, para acompanhar a explicação</summary>

```c
int node_generate_uuid(uint8_t uuid[NODE_UUID_SIZE])
{
    int random_fd;
    size_t total_read = 0U;

    if (uuid == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    random_fd = open("/dev/urandom", O_RDONLY);
    if (random_fd == -1)
    {
        return -1;
    }

    // Lê 16 bytes de /dev/urandom, tratando leituras parciais e interrupções.
    while (total_read < NODE_UUID_SIZE)
    {
        ssize_t bytes_read = read(random_fd, uuid + total_read, NODE_UUID_SIZE - total_read);

        if (bytes_read > 0)
        {
            total_read += (size_t)bytes_read;
        }
        else if (bytes_read == -1 && errno == EINTR)
        {
            continue;
        }
        else
        {
            int saved_errno = bytes_read == 0 ? EIO : errno;

            close(random_fd);
            errno = saved_errno;
            return -1;
        }
    }

    if (close(random_fd) == -1)
    {
        return -1;
    }

    // Ajusta os bits do UUID para a versão 4 e variante correta.
    uuid[6] = (uint8_t)((uuid[6] & 0x0fU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3fU) | 0x80U);
    return 0;
}
```

</details>

<a id="fn-node-c-node-config-validate"></a>

#### node_config_validate

Fonte: `node.c:105` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_config_validate(const NodeConfig *config);
```

exige porta diferente de zero, IP terminado dentro do buffer e endereço interpretável. Não testa conectividade.

**Parâmetros**

- `const NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.

**Funções do projeto usadas diretamente:** [`normalize_ip` (node.c)](#fn-node-c-normalize-ip).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`node_compute_id` (node.c)](#fn-node-c-node-compute-id); [`rpc_encode_join_payload` (rpc.c)](#fn-rpc-c-rpc-encode-join-payload); [`test_node_rejects_invalid_configuration` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-rejects-invalid-configuration).

**Retorno:** as expressões presentes nesta função são `-1`, `normalize_ip(config->ip, normalized)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_config_validate, para acompanhar a explicação</summary>

```c
int node_config_validate(const NodeConfig *config)
{
    char normalized[NODE_ADDRESS_SIZE];

    if (config == NULL || config->port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (memchr(config->ip, '\0', sizeof(config->ip)) == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    return normalize_ip(config->ip, normalized);
}
```

</details>

<a id="fn-node-c-node-config-init-with-uuid"></a>

#### node_config_init_with_uuid

Fonte: `node.c:124` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_config_init_with_uuid(NodeConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE]);
```

normaliza IP e preenche configuração usando UUID recebido. É essencial para reconstruir, no receptor, a mesma identidade anunciada no JOIN.

**Parâmetros**

- `NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `const uint8_t uuid[NODE_UUID_SIZE]`: 16 bytes persistentes usados como componente da identidade.

**Funções do projeto usadas diretamente:** [`normalize_ip` (node.c)](#fn-node-c-normalize-ip).

**Chamadores diretos encontrados nos fontes inventariados:** [`app_identity` (app_config.c)](#fn-app-config-c-app-identity); [`superpeer_config_init_with_uuid` (membership.c)](#fn-membership-c-superpeer-config-init-with-uuid); [`node_config_init` (node.c)](#fn-node-c-node-config-init); [`rpc_decode_join_payload` (rpc.c)](#fn-rpc-c-rpc-decode-join-payload); [`test_node_id_is_deterministic` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-id-is-deterministic); [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id); [`test_node_rejects_invalid_configuration` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-rejects-invalid-configuration); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`register_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-register-members).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_config_init_with_uuid, para acompanhar a explicação</summary>

```c
int node_config_init_with_uuid(NodeConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE])
{
    char normalized[NODE_ADDRESS_SIZE];

    if (config == NULL || uuid == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (normalize_ip(ip, normalized) == -1)
    {
        return -1;
    }

    // Inicializa a estrutura de configuração com o IP normalizado, porta e UUID fornecido.
    memset(config, 0, sizeof(*config));
    memcpy(config->ip, normalized, sizeof(config->ip));
    config->port = port;
    memcpy(config->uuid, uuid, NODE_UUID_SIZE);
    return 0;
}
```

</details>

<a id="fn-node-c-node-config-init"></a>

#### node_config_init

Fonte: `node.c:147` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_config_init(NodeConfig *config, const char *ip, uint16_t port);
```

gera UUID novo e chama a variante anterior. O próprio node.c não persiste UUID; app_identity, em app_config.c, fornece persistência ao Peer e ao Super Peer.

**Parâmetros**

- `NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.

**Funções do projeto usadas diretamente:** [`node_generate_uuid` (node.c)](#fn-node-c-node-generate-uuid); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_config_init` (membership.c)](#fn-membership-c-superpeer-config-init); [`legacy_command` (peer.c)](#fn-peer-c-legacy-command); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`test_node_rejects_invalid_configuration` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-rejects-invalid-configuration).

**Retorno:** as expressões presentes nesta função são `-1`, `node_config_init_with_uuid(config, ip, port, uuid)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_config_init, para acompanhar a explicação</summary>

```c
int node_config_init(NodeConfig *config, const char *ip, uint16_t port)
{
    uint8_t uuid[NODE_UUID_SIZE];

    if (config == NULL || ip == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_generate_uuid(uuid) == -1)
    {
        return -1;
    }

    // Inicializa a configuração com o IP, porta e UUID gerado.
    return node_config_init_with_uuid(config, ip, port, uuid);
}
```

</details>

<a id="fn-node-c-node-compute-id"></a>

#### node_compute_id

Fonte: `node.c:166` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_compute_id(const NodeConfig *config, NodeID *id);
```

calcula SHA-256 dos bytes binários do IP, porta em ordem de rede e UUID. PID e papel não entram no hash. Usa libcrypto.

**Parâmetros**

- `const NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `NodeID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Funções do projeto usadas diretamente:** [`node_config_validate` (node.c)](#fn-node-c-node-config-validate).

**Chamadores diretos encontrados nos fontes inventariados:** [`node_init` (node.c)](#fn-node-c-node-init); [`node_validate` (node.c)](#fn-node-c-node-validate); [`test_node_id_is_deterministic` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-id-is-deterministic).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `inet_pton`: Converte endereço textual para binário; distingue 1 válido, 0 texto inválido e -1 erro da família.
- `SHA256`: Usa OpenSSL para calcular o digest do buffer; não é implementação manual do algoritmo.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_compute_id, para acompanhar a explicação</summary>

```c
int node_compute_id(const NodeConfig *config, NodeID *id)
{
    // Valida parâmetros de entrada.
    struct in_addr ipv4;
    struct in6_addr ipv6;
    const uint8_t *address_bytes;
    size_t address_size;
    uint16_t network_port;
    uint8_t input[sizeof(struct in6_addr) + sizeof(uint16_t) + NODE_UUID_SIZE];
    size_t input_size = 0U;

    if (id == NULL || config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_validate(config) == -1)
    {
        return -1;
    }

    if (inet_pton(AF_INET, config->ip, &ipv4) == 1)
    {
        address_bytes = (const uint8_t *)&ipv4;
        address_size = sizeof(ipv4);
    }
    else if (inet_pton(AF_INET6, config->ip, &ipv6) == 1)
    {
        address_bytes = (const uint8_t *)&ipv6;
        address_size = sizeof(ipv6);
    }
    else
    {
        errno = EINVAL;
        return -1;
    }

    // Constrói o buffer de entrada para o hash: IP binário + porta em ordem de rede + UUID.
    network_port = htons(config->port);
    memcpy(input + input_size, address_bytes, address_size);
    input_size += address_size;
    memcpy(input + input_size, &network_port, sizeof(network_port));
    input_size += sizeof(network_port);
    memcpy(input + input_size, config->uuid, NODE_UUID_SIZE);
    input_size += NODE_UUID_SIZE;

    if (SHA256(input, input_size, id->bytes) == NULL)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}
```

</details>

<a id="fn-node-c-node-init"></a>

#### node_init

Fonte: `node.c:222` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_init(Node *node, const NodeConfig *config);
```

calcula NodeID, copia configuração, registra getpid local e define papel inicial PEER. O PID reconstruído no servidor é local ao servidor, não o PID remoto.

**Parâmetros**

- `Node *node`: Estrutura que reúne identidade, configuração, PID e papel de um nó.
- `const NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.

**Funções do projeto usadas diretamente:** [`node_compute_id` (node.c)](#fn-node-c-node-compute-id).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`legacy_command` (peer.c)](#fn-peer-c-legacy-command); [`join_superpeer` (peer_service.c)](#fn-peer-service-c-join-superpeer); [`initialize_service` (peer_service.c)](#fn-peer-service-c-initialize-service); [`initialize_local_identity` (superpeer_app.c)](#fn-superpeer-app-c-initialize-local-identity); [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`register_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-register-members).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_init, para acompanhar a explicação</summary>

```c
int node_init(Node *node, const NodeConfig *config)
{
    if (node == NULL || config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_compute_id(config, &node->id) == -1)
    {
        return -1;
    }

    node->config = *config;
    node->process_id = getpid();
    node->role = NODE_ROLE_PEER;
    return 0;
}
```

</details>

<a id="fn-node-c-node-validate"></a>

#### node_validate

Fonte: `node.c:241` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_validate(const Node *node);
```

recalcula o NodeID, compara-o ao armazenado e verifica PID positivo e papel permitido. Verifica consistência interna da estrutura, não autentica uma máquina externa.

**Parâmetros**

- `const Node *node`: Estrutura que reúne identidade, configuração, PID e papel de um nó.

**Funções do projeto usadas diretamente:** [`node_compute_id` (node.c)](#fn-node-c-node-compute-id); [`node_id_equal` (node.c)](#fn-node-c-node-id-equal).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_validate, para acompanhar a explicação</summary>

```c
int node_validate(const Node *node)
{
    NodeID expected_id;

    if (node == NULL || node->process_id <= 0 || node_compute_id(&node->config, &expected_id) == -1 || !node_id_equal(&node->id, &expected_id))
    {
        errno = EINVAL;
        return -1;
    }

    if (node->role != NODE_ROLE_PEER && node->role != NODE_ROLE_SUPERPEER)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}
```

</details>

<a id="fn-node-c-node-id-to-hex"></a>

#### node_id_to_hex

Fonte: `node.c:261` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_id_to_hex(const NodeID *id, char *output, size_t output_size);
```

escreve os 32 bytes como 64 dígitos hexadecimais mais NUL; exige buffer suficiente.

**Parâmetros**

- `const NodeID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `char *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.
- `size_t output_size`: Ponteiro que recebe o tamanho produzido.

**Chamadores diretos encontrados nos fontes inventariados:** [`test_node_id_is_deterministic` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-id-is-deterministic); [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOSPC`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_id_to_hex, para acompanhar a explicação</summary>

```c
int node_id_to_hex(const NodeID *id, char *output, size_t output_size)
{
    static const char hexadecimal[] = "0123456789abcdef";
    size_t i;

    if (id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (output_size < NODE_ID_HEX_SIZE)
    {
        errno = ENOSPC;
        return -1;
    }

    // Converte cada byte em dois dígitos hexadecimais e adiciona o terminador nulo.
    for (i = 0U; i < NODE_ID_SIZE; ++i)
    {
        output[i * 2U] = hexadecimal[id->bytes[i] >> 4U];
        output[i * 2U + 1U] = hexadecimal[id->bytes[i] & 0x0fU];
    }
    output[NODE_ID_HEX_SIZE - 1U] = '\0';
    return 0;
}
```

</details>

<a id="fn-node-c-hexadecimal-value"></a>

#### hexadecimal_value

Fonte: `node.c:288` (linha nesta revisão; pode mudar em futuras edições).

```c
static int hexadecimal_value(char character);
```

auxiliar privado que converte um único dígito hexadecimal, aceitando letras maiúsculas/minúsculas.

**Parâmetros**

- `char character`: Um caractere a converter para valor hexadecimal.

**Chamadores diretos encontrados nos fontes inventariados:** [`node_id_from_hex` (node.c)](#fn-node-c-node-id-from-hex).

**Retorno:** as expressões presentes nesta função são `character -`, `character - + 10`, `-1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de hexadecimal_value, para acompanhar a explicação</summary>

```c
static int hexadecimal_value(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}
```

</details>

<a id="fn-node-c-node-id-from-hex"></a>

#### node_id_from_hex

Fonte: `node.c:306` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_id_from_hex(NodeID *id, const char *hex);
```

exige 64 dígitos e reconstrói 32 bytes. Em erro a saída pode estar parcialmente preenchida; não a trate como ID válido.

**Parâmetros**

- `NodeID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `const char *hex`: 64 dígitos hexadecimais que representam um identificador.

**Funções do projeto usadas diretamente:** [`hexadecimal_value` (node.c)](#fn-node-c-hexadecimal-value).

**Chamadores diretos encontrados nos fontes inventariados:** [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de node_id_from_hex, para acompanhar a explicação</summary>

```c
int node_id_from_hex(NodeID *id, const char *hex)
{
    size_t i;

    if (id == NULL || hex == NULL || strlen(hex) != NODE_ID_HEX_SIZE - 1U)
    {
        errno = EINVAL;
        return -1;
    }

    // Converte cada par de dígitos hexadecimais em um byte binário, validando caracteres.
    for (i = 0U; i < NODE_ID_SIZE; ++i)
    {
        int high = hexadecimal_value(hex[i * 2U]);
        int low = hexadecimal_value(hex[i * 2U + 1U]);

        if (high < 0 || low < 0)
        {
            errno = EINVAL;
            return -1;
        }
        id->bytes[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}
```

</details>

<a id="fn-node-c-node-id-compare"></a>

#### node_id_compare

Fonte: `node.c:333` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_id_compare(const NodeID *left, const NodeID *right);
```

comparação lexicográfica normalizada em -1, 0 ou 1; também define ordem quando ponteiros são nulos. É um comparador, não uma eleição Bully.

**Parâmetros**

- `const NodeID *left`: Um dos identificadores binários comparados; veja tratamento explícito de NULL no corpo.
- `const NodeID *right`: Um dos identificadores binários comparados; veja tratamento explícito de NULL no corpo.

**Chamadores diretos encontrados nos fontes inventariados:** [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `0`, `-1`, `1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de node_id_compare, para acompanhar a explicação</summary>

```c
int node_id_compare(const NodeID *left, const NodeID *right)
{
    int comparison;

    if (left == NULL && right == NULL)
    {
        return 0;
    }
    if (left == NULL)
    {
        return -1;
    }
    if (right == NULL)
    {
        return 1;
    }

    comparison = memcmp(left->bytes, right->bytes, NODE_ID_SIZE);
    if (comparison < 0)
    {
        return -1;
    }
    if (comparison > 0)
    {
        return 1;
    }
    return 0;
}
```

</details>

<a id="fn-node-c-node-id-equal"></a>

#### node_id_equal

Fonte: `node.c:363` (linha nesta revisão; pode mudar em futuras edições).

```c
int node_id_equal(const NodeID *left, const NodeID *right);
```

compara os 32 bytes; ponteiros nulos não contam como IDs iguais.

**Parâmetros**

- `const NodeID *left`: Um dos identificadores binários comparados; veja tratamento explícito de NULL no corpo.
- `const NodeID *right`: Um dos identificadores binários comparados; veja tratamento explícito de NULL no corpo.

**Chamadores diretos encontrados nos fontes inventariados:** [`find_member_index_locked` (membership.c)](#fn-membership-c-find-member-index-locked); [`superpeer_unregister_node` (membership.c)](#fn-membership-c-superpeer-unregister-node); [`node_validate` (node.c)](#fn-node-c-node-validate); [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `left != NULL && right != NULL && memcmp(left->bytes, right->bytes, NODE_ID_SIZE) == 0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de node_id_equal, para acompanhar a explicação</summary>

```c
int node_id_equal(const NodeID *left, const NodeID *right)
{
    return left != NULL && right != NULL && memcmp(left->bytes, right->bytes, NODE_ID_SIZE) == 0;
}
```

</details>

<a id="fn-node-c-node-get-process-id"></a>

#### node_get_process_id

Fonte: `node.c:369` (linha nesta revisão; pode mudar em futuras edições).

```c
pid_t node_get_process_id(const Node *node);
```

devolve o PID armazenado, ou -1 para ponteiro nulo.

**Parâmetros**

- `const Node *node`: Estrutura que reúne identidade, configuração, PID e papel de um nó.

**Chamadores diretos encontrados nos fontes inventariados:** [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id).

**Retorno:** as expressões presentes nesta função são `node == NULL ? (pid_t)-1 : node->process_id`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de node_get_process_id, para acompanhar a explicação</summary>

```c
pid_t node_get_process_id(const Node *node)
{
    return node == NULL ? (pid_t)-1 : node->process_id;
}
```

</details>

<a id="mod-peer-c"></a>

### peer.c

[parse_port](#fn-peer-c-parse-port) · [message_name](#fn-peer-c-message-name) · [legacy_command](#fn-peer-c-legacy-command) · [option_command](#fn-peer-c-option-command) · [usage](#fn-peer-c-usage) · [main](#fn-peer-c-main)

<a id="fn-peer-c-parse-port"></a>

#### parse_port

Fonte: `peer.c:22` (linha nesta revisão; pode mudar em futuras edições).

```c
static int parse_port(const char *text, uint16_t *port);
```

valida porta decimal no intervalo permitido.

**Parâmetros**

- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.
- `uint16_t *port`: Porta; se ponteiro, recebe a conversão validada do texto.

**Chamadores diretos encontrados nos fontes inventariados:** [`option_command` (peer.c)](#fn-peer-c-option-command); [`main` (peer.c)](#fn-peer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `strtoul`: Converte texto decimal e informa onde a conversão terminou; a validação adicional restringe a faixa permitida.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de parse_port, para acompanhar a explicação</summary>

```c
static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || port == NULL || text[0] == '\0')
    {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > UINT16_MAX)
    {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}
```

</details>

<a id="fn-peer-c-message-name"></a>

#### message_name

Fonte: `peer.c:41` (linha nesta revisão; pode mudar em futuras edições).

```c
static const char *message_name(Message_Type type);
```

devolve rótulo legível para os comandos legados PING/JOIN/LEAVE e respostas.

**Parâmetros**

- `Message_Type type`: Código de mensagem/seletor, de acordo com o enum da assinatura.

**Chamadores diretos encontrados nos fontes inventariados:** [`legacy_command` (peer.c)](#fn-peer-c-legacy-command).

**Retorno:** as expressões presentes nesta função são ``. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de message_name, para acompanhar a explicação</summary>

```c
static const char *message_name(Message_Type type)
{
    switch (type)
    {
    case M_JOIN:
        return "JOIN";
    case M_LEAVE:
        return "LEAVE";
    case M_PING:
        return "PING";
    case M_PONG:
        return "PONG";
    case M_ACK:
        return "ACK";
    default:
        return "ERROR";
    }
}
```

</details>

<a id="fn-peer-c-legacy-command"></a>

#### legacy_command

Fonte: `peer.c:60` (linha nesta revisão; pode mudar em futuras edições).

```c
static int legacy_command(const char *command, const char *host, uint16_t port);
```

executa uma operação de compatibilidade do C1. PING envia quatro bytes e espera PONG; JOIN monta NodeID/descritor de teste e espera ACK; LEAVE espera ACK. Esse LEAVE não demonstra remoção real do membro.

**Parâmetros**

- `const char *command`: Nome textual do comando solicitado.
- `const char *host`: Endereço IPv4 textual do endpoint remoto.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.

**Funções do projeto usadas diretamente:** [`node_config_init` (node.c)](#fn-node-c-node-config-init); [`node_init` (node.c)](#fn-node-c-node-init); [`message_name` (peer.c)](#fn-peer-c-message-name); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`rpc_encode_join_payload` (rpc.c)](#fn-rpc-c-rpc-encode-join-payload); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call).

**Chamadores diretos encontrados nos fontes inventariados:** [`option_command` (peer.c)](#fn-peer-c-option-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EREMOTEIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de legacy_command, para acompanhar a explicação</summary>

```c
static int legacy_command(const char *command, const char *host, uint16_t port)
{
    Message_Type request_type;
    Message_Type expected_type;
    const uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    uint8_t join_payload[JOIN_PAYLOAD_WIRE_SIZE];
    Node local_node;
    NodeConfig config;
    const NodeID *source = NULL;
    Message response;
    int status = -1;

    if (strcmp(command, "ping") == 0)
    {
        request_type = M_PING;
        expected_type = M_PONG;
        payload = (const uint8_t *)"PING";
        payload_size = 4U;
    }
    else if (strcmp(command, "join") == 0)
    {
        request_type = M_JOIN;
        expected_type = M_ACK;
        if (node_config_init(&config, "127.0.0.1", 65534U) < 0 || node_init(&local_node, &config) < 0 || rpc_encode_join_payload(&config, join_payload) < 0)
        {
            return -1;
        }
        source = &local_node.id;
        payload = join_payload;
        payload_size = JOIN_PAYLOAD_WIRE_SIZE;
    }
    else if (strcmp(command, "leave") == 0)
    {
        request_type = M_LEAVE;
        expected_type = M_ACK;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    printf("TX %s\n", message_name(request_type));
    if (rpc_call(host, port, source, NULL, request_type, payload, payload_size, &response) < 0)
    {
        return -1;
    }
    if (response.header.message_type == (uint8_t)expected_type && (expected_type != M_PONG || (response.header.payload_size == 4U && memcmp(response.payload, "PONG", 4U) == 0)))
    {
        printf("RX %s\n", message_name(expected_type));
        status = 0;
    }
    else
    {
        errno = EREMOTEIO;
    }
    message_free(&response);
    return status;
}
```

</details>

<a id="fn-peer-c-option-command"></a>

#### option_command

Fonte: `peer.c:120` (linha nesta revisão; pode mudar em futuras edições).

```c
static int option_command(int argc, char **argv);
```

interpreta pares de opções --cmd, --host, --port, --file e --output. Para upload/download chama local_control_command; para ping/join/leave chama legacy_command. Upload/download admitem host/porta padrão; os comandos legados exigem endpoint informado.

**Parâmetros**

- `int argc`: Quantidade de argumentos; quando ponteiro, a função pode atualizar a contagem após retirar opções.
- `char **argv`: Vetor de argumentos terminados em NUL; as rotinas de configuração/CLI podem reorganizar seus ponteiros.

**Funções do projeto usadas diretamente:** [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command); [`parse_port` (peer.c)](#fn-peer-c-parse-port); [`legacy_command` (peer.c)](#fn-peer-c-legacy-command).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (peer.c)](#fn-peer-c-main).

**Retorno:** as expressões presentes nesta função são `-1`, `local_control_command(local_peer_port, 1, file, NULL, host, port)`, `local_control_command(local_peer_port, 0, file, output, host, port)`, `legacy_command(command, host, port)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de option_command, para acompanhar a explicação</summary>

```c
static int option_command(int argc, char **argv)
{
    const char *command = NULL;
    const char *host = NULL;
    const char *file = NULL;
    const char *output = NULL;
    uint16_t port = 0U;
    int index;

    for (index = 1; index < argc; index += 2)
    {
        if (index + 1 >= argc)
        {
            errno = EINVAL;
            return -1;
        }
        if (strcmp(argv[index], "--cmd") == 0)
        {
            command = argv[index + 1];
        }
        else if (strcmp(argv[index], "--host") == 0)
        {
            host = argv[index + 1];
        }
        else if (strcmp(argv[index], "--port") == 0)
        {
            if (parse_port(argv[index + 1], &port) < 0)
            {
                return -1;
            }
        }
        else if ((strcmp(argv[index], "--file") == 0 || strcmp(argv[index], "--name") == 0))
        {
            file = argv[index + 1];
        }
        else if (strcmp(argv[index], "--output") == 0)
        {
            output = argv[index + 1];
        }
        else
        {
            errno = EINVAL;
            return -1;
        }
    }
    if (command != NULL && (strcmp(command, "upload") == 0 || strcmp(command, "download") == 0))
    {
        if (host == NULL) host = "127.0.0.1";
        if (port == 0U) port = strcmp(command, "upload") == 0 ? DEFAULT_PEER_PORT : DEFAULT_SUPERPEER_PORT;
    }
    if (command == NULL || host == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(command, "upload") == 0)
    {
        if (file == NULL)
        {
            errno = EINVAL;
            return -1;
        }
        return local_control_command(local_peer_port, 1, file, NULL, host, port);
    }
    if (strcmp(command, "download") == 0)
    {
        if (file == NULL)
        {
            errno = EINVAL;
            return -1;
        }
        return local_control_command(local_peer_port, 0, file, output, host, port);
    }
    if (file != NULL || output != NULL)
    {
        errno = EINVAL;
        return -1;
    }
    return legacy_command(command, host, port);
}
```

</details>

<a id="fn-peer-c-usage"></a>

#### usage

Fonte: `peer.c:201` (linha nesta revisão; pode mudar em futuras edições).

```c
static void usage(const char *program);
```

imprime todas as formas aceitas da linha de comando.

**Parâmetros**

- `const char *program`: Nome do executável usado na mensagem de ajuda.

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (peer.c)](#fn-peer-c-main).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de usage, para acompanhar a explicação</summary>

```c
static void usage(const char *program)
{
    fprintf(stderr, "Uso:\n  %s serve <porta-peer> <host-superpeer> <porta-superpeer>\n  %s upload <arquivo.pdf> [<host-peer> <porta-peer>]\n  %s download <nome-ou-objectid> [<destino>] [<host-superpeer> <porta-superpeer>]\n  %s benchmark <arquivo.pdf>\n  %s --cmd <ping|join|leave|upload|download> --host <ip> --port <porta> [--file <arquivo>] [--output <destino>]\n", program, program, program, program, program);
}
```

</details>

<a id="fn-peer-c-main"></a>

#### main

Fonte: `peer.c:206` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(int argc, char **argv);
```

Carrega a configuração, ignora SIGPIPE e retira --local-peer-port dos argumentos. Seleciona serve, upload, download, benchmark ou a CLI legada. Upload/download vão para local_control_command, não diretamente para a rede distribuída. serve mantém o processo ativo; os comandos curtos terminam com EXIT_SUCCESS ou EXIT_FAILURE.

**Parâmetros**

- `int argc`: Quantidade de argumentos; quando ponteiro, a função pode atualizar a contagem após retirar opções.
- `char **argv`: Vetor de argumentos terminados em NUL; as rotinas de configuração/CLI podem reorganizar seus ponteiros.

**Funções do projeto usadas diretamente:** [`app_config_load` (app_config.c)](#fn-app-config-c-app-config-load); [`file_client_benchmark_lz4` (file_client.c)](#fn-file-client-c-file-client-benchmark-lz4); [`local_control_command` (local_control.c)](#fn-local-control-c-local-control-command); [`parse_port` (peer.c)](#fn-peer-c-parse-port); [`option_command` (peer.c)](#fn-peer-c-option-command); [`usage` (peer.c)](#fn-peer-c-usage); [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Retorno:** as expressões presentes nesta função são `EXIT_FAILURE`, `peer_service_run(app_config.port, app_config.superpeer, app_config.superpeer_port)`, `peer_service_run(local_port, argv[3], superpeer_port)`, `EXIT_SUCCESS`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(int argc, char **argv)
{
    int result = -1;

    if (app_config_load(&argc, argv, 0) < 0) { perror("config"); return EXIT_FAILURE; }
    (void)signal(SIGPIPE, SIG_IGN);
    /* Remove a opção transversal antes de interpretar a CLI legada. */
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--local-peer-port") == 0)
        {
            if (i + 1 >= argc || parse_port(argv[i + 1], &local_peer_port) < 0) goto failure;
            for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            argv[argc] = NULL;
            --i;
        }
    }
    if (argc >= 2 && strcmp(argv[1], "--cmd") == 0)
    {
        result = option_command(argc, argv);
    }
    else if (argc == 2 && strcmp(argv[1], "serve") == 0)
    {
        return peer_service_run(app_config.port, app_config.superpeer, app_config.superpeer_port);
    }
    else if (argc == 5 && strcmp(argv[1], "serve") == 0)
    {
        uint16_t local_port;
        uint16_t superpeer_port;

        if (parse_port(argv[2], &local_port) == 0 && parse_port(argv[4], &superpeer_port) == 0)
        {
            return peer_service_run(local_port, argv[3], superpeer_port);
        }
    }
    else if ((argc == 3 || argc == 5) && strcmp(argv[1], "upload") == 0)
    {
        const char *host = argc == 5 ? argv[3] : DEFAULT_PEER_HOST;
        uint16_t port = DEFAULT_PEER_PORT;

        if ((argc == 3 || parse_port(argv[4], &port) == 0))
        {
            result = local_control_command(local_peer_port, 1, argv[2], NULL, host, port);
        }
    }
    else if (argc == 3 && strcmp(argv[1], "benchmark") == 0)
    {
        result = file_client_benchmark_lz4(argv[2]);
    }
    else if ((argc == 3 || argc == 4 || argc == 5 || argc == 6) && strcmp(argv[1], "download") == 0)
    {
        const char *destination = NULL;
        const char *host = DEFAULT_SUPERPEER_HOST;
        uint16_t port = DEFAULT_SUPERPEER_PORT;

        if (argc == 4)
        {
            destination = argv[3];
        }
        else if (argc == 5)
        {
            host = argv[3];
            if (parse_port(argv[4], &port) < 0)
            {
                goto failure;
            }
        }
        else if (argc == 6)
        {
            destination = argv[3];
            host = argv[4];
            if (parse_port(argv[5], &port) < 0)
            {
                goto failure;
            }
        }
        result = local_control_command(local_peer_port, 0, argv[2], destination, host, port);
    }
    if (result == 0)
    {
        return EXIT_SUCCESS;
    }

failure:
    if (errno != 0)
    {
        perror("peer");
    }
    usage(argv[0]);
    return EXIT_FAILURE;
}
```

</details>

<a id="mod-peer-service-c"></a>

### peer_service.c

[stop_service](#fn-peer-service-c-stop-service) · [send_response](#fn-peer-service-c-send-response) · [join_superpeer](#fn-peer-service-c-join-superpeer) · [announce_document](#fn-peer-service-c-announce-document) · [announce_catalog](#fn-peer-service-c-announce-catalog) · [handle_store](#fn-peer-service-c-handle-store) · [handle_download](#fn-peer-service-c-handle-download) · [serve_connection](#fn-peer-service-c-serve-connection) · [initialize_service](#fn-peer-service-c-initialize-service) · [peer_service_run](#fn-peer-service-c-peer-service-run)

<a id="fn-peer-service-c-stop-service"></a>

#### stop_service

Fonte: `peer_service.c:44` (linha nesta revisão; pode mudar em futuras edições).

```c
static void stop_service(int signal_number);
```

handler de sinal que faz shutdown no socket de escuta para acordar o accept e iniciar a saída do serviço.

**Parâmetros**

- `int signal_number`: Sinal recebido pelo handler; não representa erro de rede.

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `shutdown`: Interrompe direções de comunicação do socket; o descritor ainda precisa de close.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de stop_service, para acompanhar a explicação</summary>

```c
static void stop_service(int signal_number)
{
    (void)signal_number;
    if (service_server_fd >= 0)
    {
        (void)shutdown(service_server_fd, SHUT_RDWR);
    }
}
```

</details>

<a id="fn-peer-service-c-send-response"></a>

#### send_response

Fonte: `peer_service.c:53` (linha nesta revisão; pode mudar em futuras edições).

```c
static int send_response(PeerService *service, int socket_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size);
```

constrói resposta com source_node local, destination_node da requisição e **o mesmo TransactionID**. Copia o payload fornecido, envia pelo protocolo e libera a cópia.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.
- `int socket_fd`: Descritor da conexão TCP atendida.
- `const Message *request`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.
- `Message_Type type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `uint32_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.

**Funções do projeto usadas diretamente:** [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`remote_error_encode` (remote_error.h)](#fn-remote-error-h-remote-error-encode).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store); [`handle_download` (peer_service.c)](#fn-peer-service-c-handle-download); [`serve_connection` (peer_service.c)](#fn-peer-service-c-serve-connection).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `status == PROTOCOL_OK ? 0 : -1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de send_response, para acompanhar a explicação</summary>

```c
static int send_response(PeerService *service, int socket_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size)
{
    Message response;
    int status;
    uint8_t error_payload[2];

    if (type == M_ERROR) { remote_error_encode(errno, error_payload); payload = error_payload; payload_size = sizeof(error_payload); }

    if (message_init(&response) < 0)
    {
        return -1;
    }
    response.header.message_type = (uint8_t)type;
    memcpy(response.header.source_node, service->node.id.bytes, NODE_ID_SIZE);
    memcpy(response.header.destination_node, request->header.source_node, NODE_ID_SIZE);
    memcpy(response.header.transaction_id, request->header.transaction_id, TRANSACTION_ID_SIZE);
    response.header.timestamp = (uint64_t)time(NULL);
    if (payload_size != 0U)
    {
        response.payload = malloc(payload_size);
        if (response.payload == NULL)
        {
            return -1;
        }
        memcpy(response.payload, payload, payload_size);
        response.header.payload_size = payload_size;
    }
    status = protocol_send_message(socket_fd, &response);
    message_free(&response);
    return status == PROTOCOL_OK ? 0 : -1;
}
```

</details>

<a id="fn-peer-service-c-join-superpeer"></a>

#### join_superpeer

Fonte: `peer_service.c:85` (linha nesta revisão; pode mudar em futuras edições).

```c
static int join_superpeer(PeerService *service);
```

Envia JOIN com identidade local, exige ACK, decodifica descritor remoto e recalcula NodeID. Só guarda o ID do SP se corresponder ao source_node da resposta.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.

**Funções do projeto usadas diretamente:** [`node_init` (node.c)](#fn-node-c-node-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`rpc_encode_join_payload` (rpc.c)](#fn-rpc-c-rpc-encode-join-payload); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`rpc_decode_join_payload` (rpc.c)](#fn-rpc-c-rpc-decode-join-payload).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EACCES`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de join_superpeer, para acompanhar a explicação</summary>

```c
static int join_superpeer(PeerService *service)
{
    uint8_t payload[JOIN_PAYLOAD_WIRE_SIZE];
    Message response;
    int status;

    if (rpc_encode_join_payload(&service->node.config, payload) < 0 || rpc_call(service->superpeer_host, service->superpeer_port, &service->node.id, NULL, M_JOIN, payload, JOIN_PAYLOAD_WIRE_SIZE, &response) < 0)
    {
        return -1;
    }
    status = response.header.message_type == (uint8_t)M_ACK ? 0 : -1;
    if (status == 0)
    {
        NodeConfig remote_config;
        Node remote_node;
        if (rpc_decode_join_payload(response.payload, response.header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0 || memcmp(remote_node.id.bytes, response.header.source_node, NODE_ID_SIZE) != 0) status = -1;
    }
    if (status == 0) memcpy(service->superpeer_id.bytes, response.header.source_node, NODE_ID_SIZE);
    message_free(&response);
    if (status < 0)
    {
        errno = EACCES;
    }
    return status;
}
```

</details>

<a id="fn-peer-service-c-announce-document"></a>

#### announce_document

Fonte: `peer_service.c:111` (linha nesta revisão; pode mudar em futuras edições).

```c
static int announce_document(PeerService *service, const TransferDocument *document);
```

Obtém cópia dos descritores finalizados de storage, codifica anúncio v2 e exige ACK do SP. Libera descritores/payload e preserva erro remoto específico.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.
- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Funções do projeto usadas diretamente:** [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`remote_error_decode` (remote_error.h)](#fn-remote-error-h-remote-error-decode); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`storage_descriptors` (storage.c)](#fn-storage-c-storage-descriptors); [`transfer_encode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-announcement).

**Chamadores diretos encontrados nos fontes inventariados:** [`announce_catalog` (peer_service.c)](#fn-peer-service-c-announce-catalog); [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de announce_document, para acompanhar a explicação</summary>

```c
static int announce_document(PeerService *service, const TransferDocument *document)
{
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    Message response;
    int status = -1;

    MetadataChunk *chunks = NULL;
    if (storage_descriptors(service->storage, &document->id, &chunks) < 0) return -1;
    int encoded = transfer_encode_announcement(document, chunks, &payload, &payload_size);
    free(chunks);
    if (encoded < 0 || rpc_call(service->superpeer_host, service->superpeer_port, &service->node.id, &service->superpeer_id, M_STORE, payload, payload_size, &response) < 0)
    {
        free(payload);
        return -1;
    }
    if (response.header.message_type == (uint8_t)M_ACK)
    {
        status = 0;
    }
    else
    {
        errno = remote_error_decode(response.payload, response.header.payload_size);
    }
    message_free(&response);
    free(payload);
    return status;
}
```

</details>

<a id="fn-peer-service-c-announce-catalog"></a>

#### announce_catalog

Fonte: `peer_service.c:140` (linha nesta revisão; pode mudar em futuras edições).

```c
static int announce_catalog(PeerService *service);
```

obtém storage_list e chama announce_document para cada objeto FINISHED. Reconstitui o índice volátil do Super Peer quando o Peer reinicia.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.

**Funções do projeto usadas diretamente:** [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document); [`storage_list` (storage.c)](#fn-storage-c-storage-list).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de announce_catalog, para acompanhar a explicação</summary>

```c
static int announce_catalog(PeerService *service)
{
    TransferDocument *documents = NULL;
    size_t count = 0U;
    size_t index;

    if (storage_list(service->storage, &documents, &count) < 0)
    {
        return -1;
    }
    for (index = 0U; index < count; ++index)
    {
        if (announce_document(service, &documents[index]) < 0)
        {
            free(documents);
            return -1;
        }
    }
    free(documents);
    return 0;
}
```

</details>

<a id="fn-peer-service-c-handle-store"></a>

#### handle_store

Fonte: `peer_service.c:162` (linha nesta revisão; pode mudar em futuras edições).

```c
static int handle_store(PeerService *service, int socket_fd, const Message *message);
```

despacha STORE/BEGIN para storage_begin, STORE/CHUNK para storage_put_chunk e STORE/COMMIT para storage_commit seguido de announce_document. Responde ACK apenas se a operação inteira deu certo; nos demais casos, ERROR.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.
- `int socket_fd`: Descritor da conexão TCP atendida.
- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Funções do projeto usadas diretamente:** [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit); [`transfer_decode_document` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-document); [`transfer_decode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-chunk); [`transfer_decode_object_operation` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-object-operation).

**Chamadores diretos encontrados nos fontes inventariados:** [`serve_connection` (peer_service.c)](#fn-peer-service-c-serve-connection).

**Retorno:** as expressões presentes nesta função são `send_response(service, socket_fd, message, status == 0 ? M_ACK : M_ERROR, NULL, 0U)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`, `ENOTSUP`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de handle_store, para acompanhar a explicação</summary>

```c
static int handle_store(PeerService *service, int socket_fd, const Message *message)
{
    int status = -1;

    if (message->header.payload_size == 0U)
    {
        errno = EBADMSG;
    }
    else if (message->payload[0] == TRANSFER_STORE_BEGIN)
    {
        TransferDocument document;

        if (transfer_decode_document(message->payload, message->header.payload_size, TRANSFER_STORE_BEGIN, &document) == 0)
        {
            status = storage_begin(service->storage, &document);
        }
    }
    else if (message->payload[0] == TRANSFER_STORE_CHUNK)
    {
        TransferChunk chunk;

        if (transfer_decode_chunk(message->payload, message->header.payload_size, TRANSFER_STORE_CHUNK, &chunk) == 0)
        {
            status = storage_put_chunk(service->storage, &chunk);
        }
    }
    else if (message->payload[0] == TRANSFER_STORE_COMMIT)
    {
        ObjectID id;
        TransferDocument document;

        if (transfer_decode_object_operation(message->payload, message->header.payload_size, TRANSFER_STORE_COMMIT, &id) == 0 && storage_commit(service->storage, &id, &document) == 0)
        {
            status = announce_document(service, &document);
        }
    }
    else
    {
        errno = ENOTSUP;
    }
    return send_response(service, socket_fd, message, status == 0 ? M_ACK : M_ERROR, NULL, 0U);
}
```

</details>

<a id="fn-peer-service-c-handle-download"></a>

#### handle_download

Fonte: `peer_service.c:205` (linha nesta revisão; pode mudar em futuras edições).

```c
static int handle_download(PeerService *service, int socket_fd, const Message *message);
```

decodifica ObjectID/índice, lê chunk comprimido finalizado, codifica DOWNLOAD_REP e o envia. Libera payload codificado e o buffer que veio de storage_read_chunk.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.
- `int socket_fd`: Descritor da conexão TCP atendida.
- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Funções do projeto usadas diretamente:** [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`storage_read_chunk` (storage.c)](#fn-storage-c-storage-read-chunk); [`transfer_encode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-chunk); [`transfer_decode_chunk_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-chunk-request).

**Chamadores diretos encontrados nos fontes inventariados:** [`serve_connection` (peer_service.c)](#fn-peer-service-c-serve-connection).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `send_response(service, socket_fd, message, M_ERROR, NULL, 0U)`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de handle_download, para acompanhar a explicação</summary>

```c
static int handle_download(PeerService *service, int socket_fd, const Message *message)
{
    ObjectID id;
    uint64_t index;
    TransferChunk chunk;
    uint8_t *owned_data = NULL;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    int status;

    if (transfer_decode_chunk_request(message->payload, message->header.payload_size, &id, &index) < 0 || storage_read_chunk(service->storage, &id, index, &chunk, &owned_data) < 0 || transfer_encode_chunk(&chunk, TRANSFER_DOWNLOAD_CHUNK, &payload, &payload_size) < 0)
    {
        free(owned_data);
        free(payload);
        return send_response(service, socket_fd, message, M_ERROR, NULL, 0U);
    }
    status = send_response(service, socket_fd, message, M_DOWNLOAD_REP, payload, payload_size);
    free(payload);
    free(owned_data);
    return status;
}
```

</details>

<a id="fn-peer-service-c-serve-connection"></a>

#### serve_connection

Fonte: `peer_service.c:227` (linha nesta revisão; pode mudar em futuras edições).

```c
static void serve_connection(void *context, int socket_fd);
```

callback da conexão aceita. Recebe uma Message e atende STORE, DOWNLOAD_REQ ou PING textual; qualquer outro tipo recebe ERROR. Libera a mensagem recebida.

**Parâmetros**

- `void *context`: Contexto compartilhado entregue ao callback; seu tipo real é definido pelo módulo.
- `int socket_fd`: Descritor da conexão TCP atendida.

**Funções do projeto usadas diretamente:** [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store); [`handle_download` (peer_service.c)](#fn-peer-service-c-handle-download); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `getpeername`: Obtém o endereço observado da outra ponta da conexão; não lê o NodeID do header.
- `inet_ntop`: Converte endereço binário para texto legível; não descobre identidade criptográfica.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

**Erros explícitos neste corpo:** `EACCES`, `ENOTSUP`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de serve_connection, para acompanhar a explicação</summary>

```c
static void serve_connection(void *context, int socket_fd)
{
    PeerService *service = context;
    Message message;

    message_init(&message);
    if (protocol_receive_message(socket_fd, &message) == PROTOCOL_OK)
    {
        const uint8_t zero[NODE_ID_SIZE] = {0};
        int c2 = message.header.message_type == M_STORE || message.header.message_type == M_DOWNLOAD_REQ;
        printf("Peer RX tipo=%u origem=", (unsigned)message.header.message_type);
        for (size_t i = 0U; i < NODE_ID_SIZE; ++i) printf("%02x", (unsigned)message.header.source_node[i]);
        struct sockaddr_in remote;
        socklen_t remote_size = sizeof(remote);
        char ip[INET_ADDRSTRLEN] = "desconhecido";
        if (getpeername(socket_fd, (struct sockaddr *)&remote, &remote_size) == 0) (void)inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        printf(" ip_origem=%s\n", ip);
        fflush(stdout);
        if (c2 && (memcmp(message.header.source_node, zero, NODE_ID_SIZE) == 0 || memcmp(message.header.destination_node, service->node.id.bytes, NODE_ID_SIZE) != 0))
        {
            errno = EACCES;
            (void)send_response(service, socket_fd, &message, M_ERROR, NULL, 0U);
            message_free(&message);
            return;
        }
        if (message.header.message_type == (uint8_t)M_STORE)
        {
            (void)handle_store(service, socket_fd, &message);
        }
        else if (message.header.message_type == (uint8_t)M_DOWNLOAD_REQ)
        {
            (void)handle_download(service, socket_fd, &message);
        }
        else if (message.header.message_type == (uint8_t)M_PING && message.header.payload_size == 4U && memcmp(message.payload, "PING", 4U) == 0)
        {
            (void)send_response(service, socket_fd, &message, M_PONG, (const uint8_t *)"PONG", 4U);
        }
        else
        {
            errno = ENOTSUP;
            (void)send_response(service, socket_fd, &message, M_ERROR, NULL, 0U);
        }
    }
    message_free(&message);
}
```

</details>

<a id="fn-peer-service-c-initialize-service"></a>

#### initialize_service

Fonte: `peer_service.c:273` (linha nesta revisão; pode mudar em futuras edições).

```c
static int initialize_service(PeerService *service, uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port);
```

Escolhe diretório configurado ou padrão da porta, chama app_identity e node_init e carrega storage. Não anuncia um endpoint antes de criar o listener.

**Parâmetros**

- `PeerService *service`: Contexto do Peer de armazenamento, contendo identidade, catálogo e endpoint do Super Peer.
- `uint16_t local_port`: Porta do serviço local e de identificação de seu socket Unix.
- `const char *superpeer_host`: Endereço do Super Peer consultado ou usado para JOIN.
- `uint16_t superpeer_port`: Porta TCP do Super Peer.

**Funções do projeto usadas diretamente:** [`app_identity` (app_config.c)](#fn-app-config-c-app-identity); [`node_init` (node.c)](#fn-node-c-node-init); [`storage_create` (storage.c)](#fn-storage-c-storage-create).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de initialize_service, para acompanhar a explicação</summary>

```c
static int initialize_service(PeerService *service, uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port)
{
    NodeConfig config;
    char storage_path[PEER_STORAGE_PATH_SIZE];
    int length;

    memset(service, 0, sizeof(*service));
    service->server_fd = -1;
    length = snprintf(storage_path, sizeof(storage_path), ".peer_storage/%" PRIu16, local_port);
    if (superpeer_host == NULL || length < 0 || (size_t)length >= sizeof(storage_path) || strlen(superpeer_host) >= sizeof(service->superpeer_host))
    {
        errno = EINVAL;
        return -1;
    }
    if (app_config.data_dir[0] != '\0') { strcpy(storage_path, app_config.data_dir); }
    if (app_identity(storage_path, &config, local_port) < 0 || node_init(&service->node, &config) < 0)
    {
        return -1;
    }
    strcpy(service->superpeer_host, superpeer_host);
    service->superpeer_port = superpeer_port;
    if (storage_create(storage_path, &service->node.id, &service->storage) < 0)
    {
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-peer-service-c-peer-service-run"></a>

#### peer_service_run

Fonte: `peer_service.c:301` (linha nesta revisão; pode mudar em futuras edições).

```c
int peer_service_run(uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port);
```

Inicializa storage e identidade, prepara listener antes de JOIN, reanuncia catálogo, inicia controle local e atende TCP. No fim interrompe controle, envia LEAVE somente se efetivamente entrou no SP, aguarda runtime e destrói storage.

**Parâmetros**

- `uint16_t local_port`: Porta do serviço local e de identificação de seu socket Unix.
- `const char *superpeer_host`: Endereço do Super Peer consultado ou usado para JOIN.
- `uint16_t superpeer_port`: Porta TCP do Super Peer.

**Passo a passo**

1. Inicializa a identidade persistente e carrega o catálogo do armazenamento.
2. Cria o listener antes do JOIN para não anunciar uma porta que ainda não foi aberta.
3. Faz JOIN, valida identidade do Super Peer e reanuncia documentos finalizados.
4. Inicia controle local e o runtime TCP; as requisições de armazenamento são atendidas pelo callback do Peer.
5. Ao encerrar, interrompe controle, tenta LEAVE se houve JOIN, aguarda usuários dos recursos e destrói o estado local. Os arquivos persistentes permanecem.

**Funções do projeto usadas diretamente:** [`concurrent_server_create` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-create); [`concurrent_server_run` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-run); [`concurrent_server_destroy` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-destroy); [`local_control_start` (local_control.c)](#fn-local-control-c-local-control-start); [`local_control_stop` (local_control.c)](#fn-local-control-c-local-control-stop); [`network_create_server` (network.c)](#fn-network-c-network-create-server); [`network_shutdown` (network.c)](#fn-network-c-network-shutdown); [`join_superpeer` (peer_service.c)](#fn-peer-service-c-join-superpeer); [`announce_catalog` (peer_service.c)](#fn-peer-service-c-announce-catalog); [`initialize_service` (peer_service.c)](#fn-peer-service-c-initialize-service); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (peer.c)](#fn-peer-c-main).

**Retorno:** as expressões presentes nesta função são `EXIT_FAILURE`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de peer_service_run, para acompanhar a explicação</summary>

```c
int peer_service_run(uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port)
{
    PeerService service;
    int status = EXIT_FAILURE;
    int joined = 0;

    if (initialize_service(&service, local_port, superpeer_host, superpeer_port) < 0)
    {
        perror("peer initialization");
        return EXIT_FAILURE;
    }
    service.server_fd = network_create_server(local_port, PEER_BACKLOG);
    if (service.server_fd < 0 || concurrent_server_create(service.server_fd, serve_connection, &service, &service.runtime) < 0)
    {
        goto cleanup;
    }
    if (join_superpeer(&service) < 0)
    {
        perror("peer registration");
        goto cleanup;
    }
    joined = 1;
    if (announce_catalog(&service) < 0 || local_control_start(local_port, &service.node.id, &service.control) < 0)
    {
        perror("peer registration/control");
        goto cleanup;
    }
    service_server_fd = service.server_fd;
    (void)signal(SIGINT, stop_service);
    (void)signal(SIGTERM, stop_service);
    printf("Peer storage started\nNodeID: ");
    for (size_t index = 0U; index < NODE_ID_SIZE; ++index)
    {
        printf("%02x", (unsigned)service.node.id.bytes[index]);
    }
    if (app_config.data_dir[0] != '\0') printf("\nStorage: %s\n", app_config.data_dir);
    else printf("\nStorage: .peer_storage/%" PRIu16 "\n", local_port);
    fflush(stdout);
    status = concurrent_server_run(service.runtime) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    service_server_fd = -1;
    local_control_stop(service.control);
    if (joined)
    {
        Message response;
        if (rpc_call(service.superpeer_host, service.superpeer_port, &service.node.id, &service.superpeer_id, M_LEAVE, NULL, 0U, &response) == 0) message_free(&response);
    }
    if (service.runtime != NULL)
    {
        concurrent_server_destroy(service.runtime);
    }
    else if (service.server_fd >= 0)
    {
        (void)network_shutdown(service.server_fd);
    }
    storage_destroy(service.storage);
    return status;
}
```

</details>

<a id="mod-protocol-c"></a>

### protocol.c

[crc32_update](#fn-protocol-c-crc32-update) · [protocol_message_crc32](#fn-protocol-c-protocol-message-crc32) · [message_init](#fn-protocol-c-message-init) · [message_free](#fn-protocol-c-message-free) · [protocol_serialize_header](#fn-protocol-c-protocol-serialize-header) · [protocol_deserialize_header](#fn-protocol-c-protocol-deserialize-header) · [protocol_calculate_crc32](#fn-protocol-c-protocol-calculate-crc32) · [protocol_validate_header](#fn-protocol-c-protocol-validate-header) · [protocol_send_message](#fn-protocol-c-protocol-send-message) · [protocol_receive_message](#fn-protocol-c-protocol-receive-message)

<a id="fn-protocol-c-crc32-update"></a>

#### crc32_update

Fonte: `protocol.c:21` (linha nesta revisão; pode mudar em futuras edições).

```c
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size);
```

chama crc32 da zlib em blocos que cabem no tipo uInt da biblioteca. Permite continuar o cálculo em dados longos.

**Parâmetros**

- `uint32_t crc`: Estado acumulado do CRC32 antes de acrescentar este bloco.
- `const uint8_t *data`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_message_crc32` (protocol.c)](#fn-protocol-c-protocol-message-crc32); [`protocol_calculate_crc32` (protocol.c)](#fn-protocol-c-protocol-calculate-crc32).

**Retorno:** as expressões presentes nesta função são `(uint32_t)result`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de crc32_update, para acompanhar a explicação</summary>

```c
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    uLong result = (uLong)crc;

    while (size > 0U)
    {
        uInt chunk = size > (size_t)UINT_MAX ? UINT_MAX : (uInt)size;

        result = crc32(result, data, chunk);
        data += chunk;
        size -= chunk;
    }

    return (uint32_t)result;
}
```

</details>

<a id="fn-protocol-c-protocol-message-crc32"></a>

#### protocol_message_crc32

Fonte: `protocol.c:38` (linha nesta revisão; pode mudar em futuras edições).

```c
static uint32_t protocol_message_crc32(const Header *header, const uint8_t *payload);
```

copia o header, zera seu checksum, serializa os campos canônicos e calcula CRC32 sobre header mais corpo. Assim remetente e receptor calculam exatamente a mesma sequência de bytes.

**Parâmetros**

- `const Header *header`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.

**Funções do projeto usadas diretamente:** [`crc32_update` (protocol.c)](#fn-protocol-c-crc32-update); [`protocol_serialize_header` (protocol.c)](#fn-protocol-c-protocol-serialize-header).

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message).

**Retorno:** as expressões presentes nesta função são `0`, `crc`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_message_crc32, para acompanhar a explicação</summary>

```c
static uint32_t protocol_message_crc32(const Header *header, const uint8_t *payload)
{
    Header canonical_header = *header;
    uint8_t serialized_header[HEADER_WIRE_SIZE];

    canonical_header.checksum = 0;

    if (protocol_serialize_header(&canonical_header, serialized_header, sizeof(serialized_header)) < 0)
    {
        return 0;
    }

    uint32_t crc = crc32_update(0U, serialized_header, sizeof(serialized_header));

    if (canonical_header.payload_size > 0)
    {
        crc = crc32_update(crc, payload, canonical_header.payload_size);
    }

    return crc;
}
```

</details>

<a id="fn-protocol-c-message-init"></a>

#### message_init

Fonte: `protocol.c:61` (linha nesta revisão; pode mudar em futuras edições).

```c
int message_init(Message *message);
```

zera a struct e define PROTOCOL_VERSION. Não libera um payload antigo; use message_free antes de reinicializar uma mensagem já preenchida.

**Parâmetros**

- `Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Chamadores diretos encontrados nos fontes inventariados:** [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`serve_connection` (peer_service.c)](#fn-peer-service-c-serve-connection); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply); [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`test_message_round_trip` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-message-round-trip); [`make_wire_message` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-make-wire-message); [`test_protocol_rejections` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `PROTOCOL_ERROR`, `PROTOCOL_OK`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de message_init, para acompanhar a explicação</summary>

```c
int message_init(Message *message)
{
    if (message == NULL)
    {
        return PROTOCOL_ERROR;
    }

    memset(message, 0, sizeof(*message));
    message->header.protocol_version = PROTOCOL_VERSION;
    return PROTOCOL_OK;
}
```

</details>

<a id="fn-protocol-c-message-free"></a>

#### message_free

Fonte: `protocol.c:74` (linha nesta revisão; pode mudar em futuras edições).

```c
void message_free(Message *message);
```

libera message.payload e limpa o header. É a forma normal de liberar um Message recebido ou uma resposta construída com payload próprio.

**Parâmetros**

- `Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Chamadores diretos encontrados nos fontes inventariados:** [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`discover_peer` (file_client.c)](#fn-file-client-c-discover-peer); [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker); [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload); [`download_one` (file_client.c)](#fn-file-client-c-download-one); [`lookup_document` (file_client.c)](#fn-file-client-c-lookup-document); [`legacy_command` (peer.c)](#fn-peer-c-legacy-command); [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`join_superpeer` (peer_service.c)](#fn-peer-service-c-join-superpeer); [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document); [`serve_connection` (peer_service.c)](#fn-peer-service-c-serve-connection); [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply); [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`test_message_round_trip` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-message-round-trip); [`test_protocol_rejections` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de message_free, para acompanhar a explicação</summary>

```c
void message_free(Message *message)
{
    if (message == NULL)
    {
        return;
    }

    free(message->payload);
    message->payload = NULL;
    memset(&message->header, 0, sizeof(message->header));
}
```

</details>

<a id="fn-protocol-c-protocol-serialize-header"></a>

#### protocol_serialize_header

Fonte: `protocol.c:87` (linha nesta revisão; pode mudar em futuras edições).

```c
int protocol_serialize_header(const Header *header, uint8_t *buffer, size_t buffer_size);
```

valida o header e escreve campo a campo seus 98 bytes no buffer. Retorna o número de bytes escritos ou PROTOCOL_ERROR; nunca envia sizeof(Header) diretamente.

**Parâmetros**

- `const Header *header`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `uint8_t *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t buffer_size`: Capacidade/tamanho do buffer serializado.

**Funções do projeto usadas diretamente:** [`protocol_validate_header` (protocol.c)](#fn-protocol-c-protocol-validate-header); [`wire_put_u32` (wire.h)](#fn-wire-h-wire-put-u32); [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_message_crc32` (protocol.c)](#fn-protocol-c-protocol-message-crc32); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `PROTOCOL_ERROR`, `(int)offset`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_serialize_header, para acompanhar a explicação</summary>

```c
int protocol_serialize_header(const Header *header, uint8_t *buffer, size_t buffer_size)
{
    if (header == NULL || buffer == NULL || buffer_size < HEADER_WIRE_SIZE)
    {
        return PROTOCOL_ERROR;
    }

    if (protocol_validate_header(header) < 0)
    {
        return PROTOCOL_ERROR;
    }

    size_t offset = 0;

    buffer[offset++] = header->protocol_version;
    buffer[offset++] = header->message_type;

    memcpy(buffer + offset, header->source_node, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(buffer + offset, header->destination_node, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(buffer + offset, header->transaction_id, TRANSACTION_ID_SIZE);
    offset += TRANSACTION_ID_SIZE;

    wire_put_u64(buffer + offset, header->timestamp);
    offset += sizeof(uint64_t);

    wire_put_u32(buffer + offset, header->payload_size);
    offset += sizeof(uint32_t);

    wire_put_u32(buffer + offset, header->checksum);
    offset += sizeof(uint32_t);

    return (int)offset;
}
```

</details>

<a id="fn-protocol-c-protocol-deserialize-header"></a>

#### protocol_deserialize_header

Fonte: `protocol.c:126` (linha nesta revisão; pode mudar em futuras edições).

```c
int protocol_deserialize_header(Header *header, const uint8_t *buffer, size_t buffer_size);
```

reconstrói os campos a partir dos 98 bytes. Converte números, mas **não** faz toda a validação semântica; o chamador usa protocol_validate_header depois.

**Parâmetros**

- `Header *header`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `const uint8_t *buffer`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t buffer_size`: Capacidade/tamanho do buffer serializado.

**Funções do projeto usadas diretamente:** [`wire_get_u32` (wire.h)](#fn-wire-h-wire-get-u32); [`wire_get_u64` (wire.h)](#fn-wire-h-wire-get-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `PROTOCOL_ERROR`, `(int)offset`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_deserialize_header, para acompanhar a explicação</summary>

```c
int protocol_deserialize_header(Header *header, const uint8_t *buffer, size_t buffer_size)
{
    if (header == NULL || buffer == NULL || buffer_size < HEADER_WIRE_SIZE)
    {
        return PROTOCOL_ERROR;
    }

    size_t offset = 0;

    memset(header, 0, sizeof(*header));

    header->protocol_version = buffer[offset++];
    header->message_type = buffer[offset++];

    memcpy(header->source_node, buffer + offset, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(header->destination_node, buffer + offset, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(header->transaction_id, buffer + offset, TRANSACTION_ID_SIZE);
    offset += TRANSACTION_ID_SIZE;

    header->timestamp = wire_get_u64(buffer + offset);
    offset += sizeof(uint64_t);

    header->payload_size = wire_get_u32(buffer + offset);
    offset += sizeof(uint32_t);

    header->checksum = wire_get_u32(buffer + offset);
    offset += sizeof(uint32_t);

    return (int)offset;
}
```

</details>

<a id="fn-protocol-c-protocol-calculate-crc32"></a>

#### protocol_calculate_crc32

Fonte: `protocol.c:162` (linha nesta revisão; pode mudar em futuras edições).

```c
uint32_t protocol_calculate_crc32(const uint8_t *data, size_t size);
```

calcula CRC32 simples de um buffer, útil também em testes. Retorno zero para entrada inválida não é uma sinalização de erro separada.

**Parâmetros**

- `const uint8_t *data`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Funções do projeto usadas diretamente:** [`crc32_update` (protocol.c)](#fn-protocol-c-crc32-update).

**Chamadores diretos encontrados nos fontes inventariados:** [`test_crc32_reference_vector` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-crc32-reference-vector).

**Retorno:** as expressões presentes nesta função são `0`, `crc32_update(0U, data, size)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_calculate_crc32, para acompanhar a explicação</summary>

```c
uint32_t protocol_calculate_crc32(const uint8_t *data, size_t size)
{
    if (data == NULL && size > 0)
    {
        return 0;
    }

    return crc32_update(0U, data, size);
}
```

</details>

<a id="fn-protocol-c-protocol-validate-header"></a>

#### protocol_validate_header

Fonte: `protocol.c:173` (linha nesta revisão; pode mudar em futuras edições).

```c
int protocol_validate_header(const Header *header);
```

confere versão, código de mensagem conhecido e payload_size até MAX_PAYLOAD_SIZE. Não verifica o CRC nem implementa as mensagens futuras.

**Parâmetros**

- `const Header *header`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_serialize_header` (protocol.c)](#fn-protocol-c-protocol-serialize-header); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message).

**Retorno:** as expressões presentes nesta função são `PROTOCOL_ERROR`, `PROTOCOL_OK`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_validate_header, para acompanhar a explicação</summary>

```c
int protocol_validate_header(const Header *header)
{
    if (header == NULL)
    {
        return PROTOCOL_ERROR;
    }

    if (header->protocol_version != PROTOCOL_VERSION)
    {
        return PROTOCOL_ERROR;
    }

    switch (header->message_type)
    {
    case M_JOIN:
    case M_ACK:
    case M_ERROR:
    case M_PING:
    case M_PONG:
    case M_LEAVE:
    case M_LOOKUP:
    case M_STORE:
    case M_DOWNLOAD_REQ:
    case M_DOWNLOAD_REP:
    case M_PREPARE:
    case M_COMMIT:
    case M_ABORT:
    case M_HEARTBEAT:
    case M_GOSSIP:
    case M_ELECTION:
    case M_OK:
    case M_COORDINATOR:
    case M_SNAPSHOT:
    case M_STATE_TRANSFER:
        break;
    default:
        return PROTOCOL_ERROR;
    }

    if (header->payload_size > MAX_PAYLOAD_SIZE)
    {
        return PROTOCOL_ERROR;
    }

    return PROTOCOL_OK;
}
```

</details>

<a id="fn-protocol-c-protocol-send-message"></a>

#### protocol_send_message

Fonte: `protocol.c:221` (linha nesta revisão; pode mudar em futuras edições).

```c
int protocol_send_message(int sock, const Message *message);
```

valida mensagem, calcula CRC, envia header e depois payload usando network_send_all. Devolve PROTOCOL_OK ou PROTOCOL_ERROR. O payload passado continua pertencendo ao chamador.

**Parâmetros**

- `int sock`: Descritor de socket já aberto; não contém os bytes da mensagem.
- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Funções do projeto usadas diretamente:** [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`protocol_message_crc32` (protocol.c)](#fn-protocol-c-protocol-message-crc32); [`protocol_serialize_header` (protocol.c)](#fn-protocol-c-protocol-serialize-header); [`protocol_validate_header` (protocol.c)](#fn-protocol-c-protocol-validate-header).

**Chamadores diretos encontrados nos fontes inventariados:** [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`test_message_round_trip` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-message-round-trip); [`make_wire_message` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-make-wire-message).

**Retorno:** as expressões presentes nesta função são `PROTOCOL_ERROR`, `PROTOCOL_OK`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_send_message, para acompanhar a explicação</summary>

```c
int protocol_send_message(int sock, const Message *message)
{
    if (message == NULL || protocol_validate_header(&message->header) < 0 || (message->header.payload_size > 0 && message->payload == NULL))
    {
        return PROTOCOL_ERROR;
    }

    Header wire_header = message->header;
    uint8_t serialized_header[HEADER_WIRE_SIZE];

    wire_header.checksum = 0;
    wire_header.checksum = protocol_message_crc32(&wire_header, message->payload);

    if (protocol_serialize_header(&wire_header, serialized_header, sizeof(serialized_header)) < 0)
    {
        return PROTOCOL_ERROR;
    }

    if (network_send_all(sock, serialized_header, sizeof(serialized_header)) != (ssize_t)sizeof(serialized_header))
    {
        return PROTOCOL_ERROR;
    }

    if (wire_header.payload_size > 0 && network_send_all(sock, message->payload, wire_header.payload_size) != (ssize_t)wire_header.payload_size)
    {
        return PROTOCOL_ERROR;
    }

    return PROTOCOL_OK;
}
```

</details>

<a id="fn-protocol-c-protocol-receive-message"></a>

#### protocol_receive_message

Fonte: `protocol.c:253` (linha nesta revisão; pode mudar em futuras edições).

```c
int protocol_receive_message(int sock, Message *message);
```

lê header completo, rejeita versão/tipo/tamanho inválidos, aloca e lê payload completo, recalcula CRC e só então entrega header e buffer à mensagem. Retorna PROTOCOL_CLOSED se a conexão fechou antes de começar outro header; PROTOCOL_ERROR cobre truncamento ou CRC incorreto. O chamador precisa chamar message_free.

**Parâmetros**

- `int sock`: Descritor de socket já aberto; não contém os bytes da mensagem.
- `Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Passo a passo**

1. Recebe exatamente o tamanho fixo do header antes de saber o tamanho do corpo.
2. Distingue fechamento sem novo frame de header truncado; converte os campos para a estrutura local.
3. Valida versão, tipo e limite de 5 MiB antes de alocar memória para um tamanho vindo da rede.
4. Recebe o corpo completo; recalcula CRC sobre a representação canônica com checksum zerado.
5. Só entrega a mensagem válida ao chamador. O payload recebido deverá ser liberado por message_free.

**Funções do projeto usadas diretamente:** [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact); [`protocol_message_crc32` (protocol.c)](#fn-protocol-c-protocol-message-crc32); [`protocol_deserialize_header` (protocol.c)](#fn-protocol-c-protocol-deserialize-header); [`protocol_validate_header` (protocol.c)](#fn-protocol-c-protocol-validate-header).

**Chamadores diretos encontrados nos fontes inventariados:** [`serve_connection` (peer_service.c)](#fn-peer-service-c-serve-connection); [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`test_message_round_trip` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-message-round-trip); [`test_protocol_rejections` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `PROTOCOL_ERROR`, `PROTOCOL_CLOSED`, `PROTOCOL_OK`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de protocol_receive_message, para acompanhar a explicação</summary>

```c
int protocol_receive_message(int sock, Message *message)
{
    if (message == NULL)
    {
        return PROTOCOL_ERROR;
    }

    uint8_t serialized_header[HEADER_WIRE_SIZE];
    Header received_header;
    ssize_t received = network_recv_exact(sock, serialized_header, sizeof(serialized_header));

    if (received == 0)
    {
        return PROTOCOL_CLOSED;
    }

    if (received < 0 || received != (ssize_t)sizeof(serialized_header))
    {
        return PROTOCOL_ERROR;
    }

    if (protocol_deserialize_header(&received_header, serialized_header, sizeof(serialized_header)) < 0 || protocol_validate_header(&received_header) < 0)
    {
        return PROTOCOL_ERROR;
    }

    uint8_t *payload = NULL;

    if (received_header.payload_size > 0)
    {
        payload = malloc(received_header.payload_size);
        if (payload == NULL)
        {
            return PROTOCOL_ERROR;
        }

        received = network_recv_exact(sock, payload, received_header.payload_size);

        if (received < 0 || received != (ssize_t)received_header.payload_size)
        {
            free(payload);
            return PROTOCOL_ERROR;
        }
    }

    uint32_t expected_checksum = received_header.checksum;
    uint32_t actual_checksum = protocol_message_crc32(&received_header, payload);

    if (expected_checksum != actual_checksum)
    {
        free(payload);
        return PROTOCOL_ERROR;
    }

    /* O chamador deve liberar uma mensagem anterior antes de reutilizar o objeto. */
    message->header = received_header;
    message->payload = payload;

    return PROTOCOL_OK;
}
```

</details>

<a id="mod-remote-error-h"></a>

### remote_error.h

[remote_error_encode](#fn-remote-error-h-remote-error-encode) · [remote_error_decode](#fn-remote-error-h-remote-error-decode)

<a id="fn-remote-error-h-remote-error-encode"></a>

#### remote_error_encode

Fonte: `remote_error.h:7` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline void remote_error_encode(int error, uint8_t output[2]);
```

converte errno de domínio para versão/código estáveis na rede.

**Parâmetros**

- `int error`: Código de erro a registrar ou codificar.
- `uint8_t output[2]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Chamadores diretos encontrados nos fontes inventariados:** [`send_response` (peer_service.c)](#fn-peer-service-c-send-response); [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de remote_error_encode, para acompanhar a explicação</summary>

```c
static inline void remote_error_encode(int error, uint8_t output[2])
{
    output[0] = 1U;
    switch (error)
    {
    case ENOENT: output[1] = 1U; break;
    case ENOTUNIQ: output[1] = 2U; break;
    case EEXIST: output[1] = 3U; break;
    case EBADMSG: case EINVAL: output[1] = 4U; break;
    case ENOTSUP: output[1] = 5U; break;
    case EACCES: output[1] = 6U; break;
    case ENODATA: output[1] = 7U; break;
    case EMSGSIZE: case EOVERFLOW: output[1] = 8U; break;
    default: output[1] = 9U; break;
    }
}
```

</details>

<a id="fn-remote-error-h-remote-error-decode"></a>

#### remote_error_decode

Fonte: `remote_error.h:23` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline int remote_error_decode(const uint8_t *data, size_t size);
```

traduz código wire para erro local; payload desconhecido vira EREMOTEIO.

**Parâmetros**

- `const uint8_t *data`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Chamadores diretos encontrados nos fontes inventariados:** [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document).

**Retorno:** as expressões presentes nesta função são `EREMOTEIO`, `ENOENT`, `ENOTUNIQ`, `EEXIST`, `EBADMSG`, `ENOTSUP`, `EACCES`, `ENODATA`, `EMSGSIZE`, `EIO`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de remote_error_decode, para acompanhar a explicação</summary>

```c
static inline int remote_error_decode(const uint8_t *data, size_t size)
{
    if (data == NULL || size != 2U || data[0] != 1U) return EREMOTEIO;
    switch (data[1])
    {
    case 1U: return ENOENT;
    case 2U: return ENOTUNIQ;
    case 3U: return EEXIST;
    case 4U: return EBADMSG;
    case 5U: return ENOTSUP;
    case 6U: return EACCES;
    case 7U: return ENODATA;
    case 8U: return EMSGSIZE;
    default: return EIO;
    }
}
```

</details>

<a id="mod-rpc-c"></a>

### rpc.c

[rpc_encode_join_payload](#fn-rpc-c-rpc-encode-join-payload) · [rpc_call](#fn-rpc-c-rpc-call) · [rpc_decode_join_payload](#fn-rpc-c-rpc-decode-join-payload)

<a id="fn-rpc-c-rpc-encode-join-payload"></a>

#### rpc_encode_join_payload

Fonte: `rpc.c:12` (linha nesta revisão; pode mudar em futuras edições).

```c
int rpc_encode_join_payload(const NodeConfig *config, uint8_t output[JOIN_PAYLOAD_WIRE_SIZE]);
```

valida NodeConfig e monta os 64 bytes do descritor JOIN: IP textual/padding, porta em ordem de rede e UUID. Essa representação não inclui PID nem envia a struct C crua.

**Parâmetros**

- `const NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `uint8_t output[JOIN_PAYLOAD_WIRE_SIZE]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`node_config_validate` (node.c)](#fn-node-c-node-config-validate).

**Chamadores diretos encontrados nos fontes inventariados:** [`legacy_command` (peer.c)](#fn-peer-c-legacy-command); [`join_superpeer` (peer_service.c)](#fn-peer-service-c-join-superpeer); [`encode_join_payload_alloc` (superpeer_app.c)](#fn-superpeer-app-c-encode-join-payload-alloc).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de rpc_encode_join_payload, para acompanhar a explicação</summary>

```c
int rpc_encode_join_payload(const NodeConfig *config, uint8_t output[JOIN_PAYLOAD_WIRE_SIZE])
{
    uint16_t port;

    if (config == NULL || output == NULL || node_config_validate(config) < 0)
    {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, JOIN_PAYLOAD_WIRE_SIZE);
    memcpy(output, config->ip, NODE_ADDRESS_SIZE);
    port = htons(config->port);
    memcpy(output + NODE_ADDRESS_SIZE, &port, sizeof(port));
    memcpy(output + NODE_ADDRESS_SIZE + sizeof(port), config->uuid, NODE_UUID_SIZE);
    return 0;
}
```

</details>

<a id="fn-rpc-c-rpc-call"></a>

#### rpc_call

Fonte: `rpc.c:29` (linha nesta revisão; pode mudar em futuras edições).

```c
int rpc_call(const char *host, uint16_t port, const NodeID *source, const NodeID *destination, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response);
```

Recebe origem/destino explícitos quando conhecidos, abre TCP, envia frame e verifica TransactionID, destino da resposta e origem remota esperada. Fecha a conexão em todos os caminhos. Payload recebido pertence ao chamador.

**Parâmetros**

- `const char *host`: Endereço IPv4 textual do endpoint remoto.
- `uint16_t port`: Porta; se ponteiro, recebe a conversão validada do texto.
- `const NodeID *source`: NodeID da origem da RPC; nulo significa identidade não informada no modo legado.
- `const NodeID *destination`: NodeID remoto esperado; nulo é permitido para descoberta inicial.
- `Message_Type type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `uint32_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `Message *response`: Message de saída; o payload recebido precisa de message_free quando a chamada termina com sucesso.

**Passo a passo**

1. Inicializa duas Message e abre uma conexão TCP para uma única chamada.
2. Preenche tipo, IDs disponíveis, TransactionID, timestamp e tamanho. O payload de entrada é emprestado, não transferido.
3. Envia o pedido e recebe a resposta pelo protocolo de framing/CRC.
4. Confere TransactionID e, quando informados, origem e destino esperados. O tipo específico é conferido pelas funções superiores, como request_expect.
5. Fecha TCP em qualquer caminho. Libera resposta se inválida; em sucesso o chamador assume o payload da resposta.

**Funções do projeto usadas diretamente:** [`network_connect` (network.c)](#fn-network-c-network-connect); [`network_shutdown` (network.c)](#fn-network-c-network-shutdown); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`transfer_fill_transaction_id` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-fill-transaction-id).

**Chamadores diretos encontrados nos fontes inventariados:** [`request_expect` (file_client.c)](#fn-file-client-c-request-expect); [`legacy_command` (peer.c)](#fn-peer-c-legacy-command); [`join_superpeer` (peer_service.c)](#fn-peer-service-c-join-superpeer); [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document); [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de rpc_call, para acompanhar a explicação</summary>

```c
int rpc_call(const char *host, uint16_t port, const NodeID *source, const NodeID *destination, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response)
{
    Message request;
    int socket_fd;
    int status = -1;

    if (host == NULL || response == NULL || payload_size > MAX_PAYLOAD_SIZE || (payload_size != 0U && payload == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    if (message_init(&request) < 0 || message_init(response) < 0)
    {
        return -1;
    }
    socket_fd = network_connect(host, port);
    if (socket_fd < 0)
    {
        return -1;
    }
    request.header.message_type = (uint8_t)type;
    if (source != NULL)
    {
        memcpy(request.header.source_node, source->bytes, NODE_ID_SIZE);
    }
    if (destination != NULL)
    {
        memcpy(request.header.destination_node, destination->bytes, NODE_ID_SIZE);
    }
    transfer_fill_transaction_id(request.header.transaction_id, request.header.source_node);
    request.header.timestamp = (uint64_t)time(NULL);
    request.header.payload_size = payload_size;
    request.payload = (uint8_t *)payload;
    errno = 0;
    if (protocol_send_message(socket_fd, &request) != PROTOCOL_OK || protocol_receive_message(socket_fd, response) != PROTOCOL_OK)
    {
        if (errno == 0) errno = EBADMSG;
        goto cleanup;
    }
    if (memcmp(request.header.transaction_id, response->header.transaction_id, TRANSACTION_ID_SIZE) != 0)
    {
        errno = EBADMSG;
        goto cleanup;
    }
    status = 0;
    if ((source != NULL && memcmp(response->header.destination_node, source->bytes, NODE_ID_SIZE) != 0) || (destination != NULL && memcmp(response->header.source_node, destination->bytes, NODE_ID_SIZE) != 0))
    {
        errno = EBADMSG;
        status = -1;
    }

cleanup:
    request.payload = NULL;
    { int error = errno; (void)network_shutdown(socket_fd); errno = error; }
    if (status < 0)
    {
        message_free(response);
    }
    return status;
}
```

</details>

<a id="fn-rpc-c-rpc-decode-join-payload"></a>

#### rpc_decode_join_payload

Fonte: `rpc.c:90` (linha nesta revisão; pode mudar em futuras edições).

```c
int rpc_decode_join_payload(const uint8_t *payload, size_t payload_size, NodeConfig *config);
```

exige tamanho de JOIN, IP terminado e padding zero; converte porta e valida NodeConfig.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.

**Funções do projeto usadas diretamente:** [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid).

**Chamadores diretos encontrados nos fontes inventariados:** [`join_superpeer` (peer_service.c)](#fn-peer-service-c-join-superpeer); [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de rpc_decode_join_payload, para acompanhar a explicação</summary>

```c
int rpc_decode_join_payload(const uint8_t *payload, size_t payload_size, NodeConfig *config)
{
    char ip[NODE_ADDRESS_SIZE];
    const uint8_t *terminator;
    uint16_t network_port;
    uint16_t port;
    size_t index;

    if (payload == NULL || config == NULL || payload_size != JOIN_PAYLOAD_WIRE_SIZE)
    {
        errno = EINVAL;
        return -1;
    }

    terminator = memchr(payload, '\0', NODE_ADDRESS_SIZE);
    if (terminator == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    /* O preenchimento após o terminador deve conter apenas zeros. */
    index = (size_t)(terminator - payload) + 1U;
    while (index < NODE_ADDRESS_SIZE)
    {
        if (payload[index] != 0U)
        {
            errno = EINVAL;
            return -1;
        }
        ++index;
    }

    memcpy(ip, payload, sizeof(ip));
    memcpy(&network_port, payload + NODE_ADDRESS_SIZE, sizeof(network_port));
    port = ntohs(network_port);

    if (node_config_init_with_uuid( config, ip, port, payload + NODE_ADDRESS_SIZE + sizeof(network_port)) < 0)
    {
        return -1;
    }
    return 0;
}
```

</details>

<a id="mod-storage-c"></a>

### storage.c

[allocate_document](#fn-storage-c-allocate-document) · [release_document](#fn-storage-c-release-document) · [sync_parent](#fn-storage-c-sync-parent) · [write_all_fd](#fn-storage-c-write-all-fd) · [read_all_fd](#fn-storage-c-read-all-fd) · [ensure_directory_tree](#fn-storage-c-ensure-directory-tree) · [object_hex](#fn-storage-c-object-hex) · [document_directory](#fn-storage-c-document-directory) · [chunk_path](#fn-storage-c-chunk-path) · [find_document](#fn-storage-c-find-document) · [write_manifest](#fn-storage-c-write-manifest) · [read_manifest](#fn-storage-c-read-manifest) · [load_documents](#fn-storage-c-load-documents) · [storage_create](#fn-storage-c-storage-create) · [storage_destroy](#fn-storage-c-storage-destroy) · [storage_begin](#fn-storage-c-storage-begin) · [storage_put_chunk](#fn-storage-c-storage-put-chunk) · [verify_document](#fn-storage-c-verify-document) · [storage_commit](#fn-storage-c-storage-commit) · [storage_find](#fn-storage-c-storage-find) · [storage_read_chunk](#fn-storage-c-storage-read-chunk) · [storage_list](#fn-storage-c-storage-list) · [storage_descriptors](#fn-storage-c-storage-descriptors)

<a id="fn-storage-c-allocate-document"></a>

#### allocate_document

Fonte: `storage.c:61` (linha nesta revisão; pode mudar em futuras edições).

```c
static StoredDocument *allocate_document(void);
```

Aloca um StoredDocument zerado, inicializa o estado atômico e seu mutex. Se o mutex não puder ser criado, libera o registro e propaga a falha. O vetor de chunks será preenchido posteriormente.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Chamadores diretos encontrados nos fontes inventariados:** [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `NULL`, `document`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de allocate_document, para acompanhar a explicação</summary>

```c
static StoredDocument *allocate_document(void)
{
    StoredDocument *document = calloc(1U, sizeof(*document));
    if (document == NULL) return NULL;
    int error = pthread_mutex_init(&document->mutex, NULL);
    if (error != 0) { free(document); errno = error; return NULL; }
    return document;
}
```

</details>

<a id="fn-storage-c-release-document"></a>

#### release_document

Fonte: `storage.c:70` (linha nesta revisão; pode mudar em futuras edições).

```c
static void release_document(StoredDocument *document);
```

Destrói o mutex e libera o registro StoredDocument. O vetor de chunks precisa ser liberado pelo chamador; essa função não apaga diretórios nem chunks do disco.

**Parâmetros**

- `StoredDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Chamadores diretos encontrados nos fontes inventariados:** [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`load_documents` (storage.c)](#fn-storage-c-load-documents); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de release_document, para acompanhar a explicação</summary>

```c
static void release_document(StoredDocument *document)
{
    if (document != NULL) { pthread_mutex_destroy(&document->mutex); free(document); }
}
```

</details>

<a id="fn-storage-c-sync-parent"></a>

#### sync_parent

Fonte: `storage.c:76` (linha nesta revisão; pode mudar em futuras edições).

```c
static int sync_parent(const char *path);
```

sincroniza diretório pai de manifest/chunk ou pasta publicada.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Chamadores diretos encontrados nos fontes inventariados:** [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `fsync`: Solicita sincronização do descritor no armazenamento; após rename/link, sincronizar o diretório pai também importa.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de sync_parent, para acompanhar a explicação</summary>

```c
static int sync_parent(const char *path)
{
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == NULL) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int status = fsync(fd);
    int error = errno;
    close(fd);
    errno = error;
    return status;
}
```

</details>

<a id="fn-storage-c-write-all-fd"></a>

#### write_all_fd

Fonte: `storage.c:102` (linha nesta revisão; pode mudar em futuras edições).

```c
static int write_all_fd(int fd, const uint8_t *data, size_t size);
```

repete write até gravar tudo, tratando EINTR e escrita parcial.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `const uint8_t *data`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Chamadores diretos encontrados nos fontes inventariados:** [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `write`: Pode gravar apenas parte dos bytes; sucesso parcial não conclui automaticamente a operação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EIO`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de write_all_fd, para acompanhar a explicação</summary>

```c
static int write_all_fd(int fd, const uint8_t *data, size_t size)
{
    size_t written = 0U;

    while (written < size)
    {
        ssize_t result = write(fd, data + written, size - written);

        if (result > 0)
        {
            written += (size_t)result;
        }
        else if (result < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            if (result == 0)
            {
                errno = EIO;
            }
            return -1;
        }
    }
    return 0;
}
```

</details>

<a id="fn-storage-c-read-all-fd"></a>

#### read_all_fd

Fonte: `storage.c:130` (linha nesta revisão; pode mudar em futuras edições).

```c
static int read_all_fd(int fd, uint8_t *data, size_t size);
```

repete read até completar. EOF prematuro torna o manifest ou chunk inválido.

**Parâmetros**

- `int fd`: Descritor de arquivo/socket sobre o qual a operação atua.
- `uint8_t *data`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Chamadores diretos encontrados nos fontes inventariados:** [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`verify_document` (storage.c)](#fn-storage-c-verify-document); [`storage_read_chunk` (storage.c)](#fn-storage-c-storage-read-chunk).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `read`: Pode devolver menos bytes que o solicitado, zero em EOF ou -1 em erro.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de read_all_fd, para acompanhar a explicação</summary>

```c
static int read_all_fd(int fd, uint8_t *data, size_t size)
{
    size_t received = 0U;

    while (received < size)
    {
        ssize_t result = read(fd, data + received, size - received);

        if (result > 0)
        {
            received += (size_t)result;
        }
        else if (result < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            errno = result == 0 ? EBADMSG : errno;
            return -1;
        }
    }
    return 0;
}
```

</details>

<a id="fn-storage-c-ensure-directory-tree"></a>

#### ensure_directory_tree

Fonte: `storage.c:155` (linha nesta revisão; pode mudar em futuras edições).

```c
static int ensure_directory_tree(const char *path);
```

cria componentes do caminho com permissão 0700, tolerando EEXIST. Usado para raiz, pending e objects.

**Parâmetros**

- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.

**Chamadores diretos encontrados nos fontes inventariados:** [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`storage_create` (storage.c)](#fn-storage-c-storage-create); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de ensure_directory_tree, para acompanhar a explicação</summary>

```c
static int ensure_directory_tree(const char *path)
{
    char copy[PATH_MAX];
    char *cursor;
    size_t length;

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    length = strnlen(path, sizeof(copy));
    if (length == 0U || length >= sizeof(copy))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(copy, path, length + 1U);
    for (cursor = copy + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            *cursor = '\0';
            if (mkdir(copy, 0700) < 0 && errno != EEXIST)
            {
                return -1;
            }
            *cursor = '/';
        }
    }
    if (mkdir(copy, 0700) < 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-storage-c-object-hex"></a>

#### object_hex

Fonte: `storage.c:192` (linha nesta revisão; pode mudar em futuras edições).

```c
static int object_hex(const ObjectID *id, char output[OBJECT_ID_HEX_SIZE]);
```

wrapper de object_id_to_hex para obter nome seguro do diretório.

**Parâmetros**

- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `char output[OBJECT_ID_HEX_SIZE]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`object_id_to_hex` (metadata.c)](#fn-metadata-c-object-id-to-hex).

**Chamadores diretos encontrados nos fontes inventariados:** [`document_directory` (storage.c)](#fn-storage-c-document-directory); [`load_documents` (storage.c)](#fn-storage-c-load-documents).

**Retorno:** as expressões presentes nesta função são `object_id_to_hex(id, output, OBJECT_ID_HEX_SIZE)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de object_hex, para acompanhar a explicação</summary>

```c
static int object_hex(const ObjectID *id, char output[OBJECT_ID_HEX_SIZE])
{
    return object_id_to_hex(id, output, OBJECT_ID_HEX_SIZE);
}
```

</details>

<a id="fn-storage-c-document-directory"></a>

#### document_directory

Fonte: `storage.c:197` (linha nesta revisão; pode mudar em futuras edições).

```c
static int document_directory(const Storage *storage, const ObjectID *id, int final, char output[PATH_MAX]);
```

constrói caminho de pending ou objects para um ObjectID, validando o comprimento.

**Parâmetros**

- `const Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `int final`: Seleciona área objects (final) ou pending (pendente); não é retorno da função.
- `char output[PATH_MAX]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`object_hex` (storage.c)](#fn-storage-c-object-hex).

**Chamadores diretos encontrados nos fontes inventariados:** [`chunk_path` (storage.c)](#fn-storage-c-chunk-path); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de document_directory, para acompanhar a explicação</summary>

```c
static int document_directory(const Storage *storage, const ObjectID *id, int final, char output[PATH_MAX])
{
    char hex[OBJECT_ID_HEX_SIZE];
    int length;

    if (object_hex(id, hex) < 0)
    {
        return -1;
    }
    length = snprintf(output, PATH_MAX, "%s/%s/%s", storage->root, final ? "objects" : "pending", hex);
    if (length < 0 || (size_t)length >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-storage-c-chunk-path"></a>

#### chunk_path

Fonte: `storage.c:215` (linha nesta revisão; pode mudar em futuras edições).

```c
static int chunk_path(const Storage *storage, const ObjectID *id, uint64_t index, int final, char output[PATH_MAX]);
```

acrescenta o nome determinístico do chunk ao diretório do objeto.

**Parâmetros**

- `const Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.
- `int final`: Seleciona área objects (final) ou pending (pendente); não é retorno da função.
- `char output[PATH_MAX]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`document_directory` (storage.c)](#fn-storage-c-document-directory).

**Chamadores diretos encontrados nos fontes inventariados:** [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`verify_document` (storage.c)](#fn-storage-c-verify-document); [`storage_read_chunk` (storage.c)](#fn-storage-c-storage-read-chunk).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de chunk_path, para acompanhar a explicação</summary>

```c
static int chunk_path(const Storage *storage, const ObjectID *id, uint64_t index, int final, char output[PATH_MAX])
{
    char directory[PATH_MAX];
    int length;

    if (document_directory(storage, id, final, directory) < 0)
    {
        return -1;
    }
    length = snprintf(output, PATH_MAX, "%s/chunk-%020" PRIu64 ".lz4", directory, index);
    if (length < 0 || (size_t)length >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-storage-c-find-document"></a>

#### find_document

Fonte: `storage.c:233` (linha nesta revisão; pode mudar em futuras edições).

```c
static StoredDocument *find_document(Storage *storage, const ObjectID *id);
```

busca linear no catálogo em memória; usada sob o mutex do Storage.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Chamadores diretos encontrados nos fontes inventariados:** [`load_documents` (storage.c)](#fn-storage-c-load-documents); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit); [`storage_read_chunk` (storage.c)](#fn-storage-c-storage-read-chunk); [`storage_descriptors` (storage.c)](#fn-storage-c-storage-descriptors).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `document`, `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de find_document, para acompanhar a explicação</summary>

```c
static StoredDocument *find_document(Storage *storage, const ObjectID *id)
{
    StoredDocument *document;

    for (document = storage->documents; document != NULL; document = document->next)
    {
        if (memcmp(document->document.id.bytes, id->bytes, OBJECT_ID_SIZE) == 0)
        {
            return document;
        }
    }
    return NULL;
}
```

</details>

<a id="fn-storage-c-write-manifest"></a>

#### write_manifest

Fonte: `storage.c:247` (linha nesta revisão; pode mudar em futuras edições).

```c
static int write_manifest(Storage *storage, StoredDocument *stored, int final);
```

serializa magic, versão, documento, proprietário, estado e cada descritor para arquivo temporário; executa fsync e rename para publicar o manifest. Em falha remove o temporário.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `StoredDocument *stored`: Registro interno do armazenamento; contém estado, descritores e mutex por documento.
- `int final`: Seleciona área objects (final) ou pending (pendente); não é retorno da função.

**Funções do projeto usadas diretamente:** [`sync_parent` (storage.c)](#fn-storage-c-sync-parent); [`write_all_fd` (storage.c)](#fn-storage-c-write-all-fd); [`ensure_directory_tree` (storage.c)](#fn-storage-c-ensure-directory-tree); [`document_directory` (storage.c)](#fn-storage-c-document-directory); [`wire_put_u16` (wire.h)](#fn-wire-h-wire-put-u16); [`wire_put_u32` (wire.h)](#fn-wire-h-wire-put-u32); [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`load_documents` (storage.c)](#fn-storage-c-load-documents); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `fsync`: Solicita sincronização do descritor no armazenamento; após rename/link, sincronizar o diretório pai também importa.
- `rename`: Muda o nome/localização de uma entrada no sistema de arquivos. A atomicidade dessa mudança não engloba outras operações de rede.
- `unlink`: Remove um nome de arquivo/socket. Não é exclusão recursiva e não deve atingir arquivo que a operação não criou.

**Retorno:** as expressões presentes nesta função são `-1`, `result`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de write_manifest, para acompanhar a explicação</summary>

```c
static int write_manifest(Storage *storage, StoredDocument *stored, int final)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char temporary[PATH_MAX];
    uint8_t scalar[8];
    size_t name_size = strlen(stored->document.name);
    uint64_t index;
    int fd = -1;
    int result = -1;
    int length;

    if (document_directory(storage, &stored->document.id, final, directory) < 0 || ensure_directory_tree(directory) < 0)
    {
        return -1;
    }
    length = snprintf(path, sizeof(path), "%s/manifest.bin", directory);
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    length = snprintf(temporary, sizeof(temporary), "%s/manifest.tmp-%ld", directory, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return -1;
    }
    if (write_all_fd(fd, (const uint8_t *)MANIFEST_MAGIC, MANIFEST_MAGIC_SIZE) < 0)
    {
        goto cleanup;
    }
    wire_put_u32(scalar, MANIFEST_VERSION);
    if (write_all_fd(fd, scalar, 4U) < 0 || write_all_fd(fd, stored->document.id.bytes, OBJECT_ID_SIZE) < 0)
    {
        goto cleanup;
    }
    wire_put_u64(scalar, stored->document.file_size);
    if (write_all_fd(fd, scalar, 8U) < 0)
    {
        goto cleanup;
    }
    wire_put_u64(scalar, stored->document.chunk_count);
    if (write_all_fd(fd, scalar, 8U) < 0 || write_all_fd(fd, &stored->document.compression, 1U) < 0)
    {
        goto cleanup;
    }
    wire_put_u16(scalar, (uint16_t)name_size);
    if (write_all_fd(fd, scalar, 2U) < 0 || write_all_fd(fd, (const uint8_t *)stored->document.name, name_size) < 0 || write_all_fd(fd, stored->owner.bytes, NODE_ID_SIZE) < 0)
    {
        goto cleanup;
    }
    wire_put_u64(scalar, stored->uploaded_at);
    if (write_all_fd(fd, scalar, 8U) < 0)
    {
        goto cleanup;
    }
    scalar[0] = (uint8_t)(stored->state == TRANSFER_VERIFYING ? TRANSFER_FINISHED : stored->state);
    if (write_all_fd(fd, scalar, 1U) < 0)
    {
        goto cleanup;
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        StoredChunk *chunk = &stored->chunks[index];

        wire_put_u64(scalar, chunk->index);
        if (write_all_fd(fd, scalar, 8U) < 0)
        {
            goto cleanup;
        }
        wire_put_u64(scalar, chunk->offset);
        if (write_all_fd(fd, scalar, 8U) < 0)
        {
            goto cleanup;
        }
        wire_put_u32(scalar, chunk->raw_size);
        if (write_all_fd(fd, scalar, 4U) < 0)
        {
            goto cleanup;
        }
        wire_put_u32(scalar, chunk->compressed_size);
        if (write_all_fd(fd, scalar, 4U) < 0 || write_all_fd(fd, chunk->hash, OBJECT_ID_SIZE) < 0)
        {
            goto cleanup;
        }
        scalar[0] = (uint8_t)(chunk->present != 0);
        if (write_all_fd(fd, scalar, 1U) < 0)
        {
            goto cleanup;
        }
    }
    if (fsync(fd) < 0)
    {
        goto cleanup;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        goto cleanup;
    }
    fd = -1;
    if (rename(temporary, path) < 0 || sync_parent(path) < 0)
    {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (fd >= 0)
    {
        close(fd);
    }
    if (result < 0)
    {
        unlink(temporary);
    }
    return result;
}
```

</details>

<a id="fn-storage-c-read-manifest"></a>

#### read_manifest

Fonte: `storage.c:372` (linha nesta revisão; pode mudar em futuras edições).

```c
static int read_manifest(Storage *storage, const char *path, int final, StoredDocument **output);
```

lê/valida magic, versão, tamanhos, nome, estado e descritores; compara tamanhos reais dos chunks no disco. Para objeto finalizado, chama verify_document, que recalcula os hashes. Só devolve catálogo alocado quando tudo é coerente.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const char *path`: Caminho de arquivo/diretório a acessar; consulte as flags de abertura para efeitos exatos.
- `int final`: Seleciona área objects (final) ou pending (pendente); não é retorno da função.
- `StoredDocument **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Funções do projeto usadas diretamente:** [`compression_lz4_bound` (compression.c)](#fn-compression-c-compression-lz4-bound); [`allocate_document` (storage.c)](#fn-storage-c-allocate-document); [`release_document` (storage.c)](#fn-storage-c-release-document); [`read_all_fd` (storage.c)](#fn-storage-c-read-all-fd); [`chunk_path` (storage.c)](#fn-storage-c-chunk-path); [`verify_document` (storage.c)](#fn-storage-c-verify-document); [`wire_get_u16` (wire.h)](#fn-wire-h-wire-get-u16); [`wire_get_u32` (wire.h)](#fn-wire-h-wire-get-u32); [`wire_get_u64` (wire.h)](#fn-wire-h-wire-get-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`load_documents` (storage.c)](#fn-storage-c-load-documents).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `read`: Pode devolver menos bytes que o solicitado, zero em EOF ou -1 em erro.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de read_manifest, para acompanhar a explicação</summary>

```c
static int read_manifest(Storage *storage, const char *path, int final, StoredDocument **output)
{
    uint8_t scalar[8];
    uint8_t magic[MANIFEST_MAGIC_SIZE];
    StoredDocument *stored = NULL;
    uint16_t name_size;
    uint64_t index;
    int fd = -1;
    ssize_t trailing;

    *output = NULL;
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    stored = allocate_document();
    if (stored == NULL)
    {
        goto error;
    }
    if (read_all_fd(fd, magic, sizeof(magic)) < 0 || memcmp(magic, MANIFEST_MAGIC, sizeof(magic)) != 0 || read_all_fd(fd, scalar, 4U) < 0 || wire_get_u32(scalar) != MANIFEST_VERSION || read_all_fd(fd, stored->document.id.bytes, OBJECT_ID_SIZE) < 0 || read_all_fd(fd, scalar, 8U) < 0)
    {
        errno = EBADMSG;
        goto error;
    }
    stored->document.file_size = wire_get_u64(scalar);
    if (read_all_fd(fd, scalar, 8U) < 0)
    {
        goto error;
    }
    stored->document.chunk_count = wire_get_u64(scalar);
    if (stored->document.chunk_count != stored->document.file_size / METADATA_CHUNK_SIZE + (stored->document.file_size % METADATA_CHUNK_SIZE != 0U) || stored->document.chunk_count > SIZE_MAX / sizeof(*stored->chunks) || read_all_fd(fd, &stored->document.compression, 1U) < 0 || stored->document.compression != COMPRESSION_LZ4 || read_all_fd(fd, scalar, 2U) < 0)
    {
        errno = EBADMSG;
        goto error;
    }
    name_size = wire_get_u16(scalar);
    if (name_size == 0U || name_size >= METADATA_NAME_SIZE || read_all_fd(fd, (uint8_t *)stored->document.name, name_size) < 0 || memchr(stored->document.name, '\0', name_size) != NULL || memchr(stored->document.name, '/', name_size) != NULL)
    {
        errno = EBADMSG;
        goto error;
    }
    stored->document.name[name_size] = '\0';
    if (read_all_fd(fd, stored->owner.bytes, NODE_ID_SIZE) < 0 || read_all_fd(fd, scalar, 8U) < 0)
    {
        goto error;
    }
    stored->uploaded_at = wire_get_u64(scalar);
    if (read_all_fd(fd, scalar, 1U) < 0 || scalar[0] > TRANSFER_REPLICATED || (final && scalar[0] != TRANSFER_FINISHED && scalar[0] != TRANSFER_VERIFYING) || (!final && scalar[0] == TRANSFER_FINISHED))
    {
        errno = EBADMSG;
        goto error;
    }
    stored->state = (TransferState)scalar[0];
    if (stored->document.chunk_count != 0U)
    {
        stored->chunks = calloc((size_t)stored->document.chunk_count, sizeof(*stored->chunks));
        if (stored->chunks == NULL)
        {
            goto error;
        }
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        StoredChunk *chunk = &stored->chunks[index];
        char chunk_file[PATH_MAX];

        if (read_all_fd(fd, scalar, 8U) < 0)
        {
            goto error;
        }
        chunk->index = wire_get_u64(scalar);
        if (read_all_fd(fd, scalar, 8U) < 0)
        {
            goto error;
        }
        chunk->offset = wire_get_u64(scalar);
        if (read_all_fd(fd, scalar, 4U) < 0)
        {
            goto error;
        }
        chunk->raw_size = wire_get_u32(scalar);
        if (read_all_fd(fd, scalar, 4U) < 0)
        {
            goto error;
        }
        chunk->compressed_size = wire_get_u32(scalar);
        if (read_all_fd(fd, chunk->hash, OBJECT_ID_SIZE) < 0 || read_all_fd(fd, scalar, 1U) < 0)
        {
            goto error;
        }
        chunk->present = scalar[0] != 0U;
        if (scalar[0] > 1U || (final && !chunk->present))
        {
            errno = EBADMSG;
            goto error;
        }
        if (chunk->present)
        {
            uint64_t remaining = stored->document.file_size - index * METADATA_CHUNK_SIZE;
            uint32_t expected_size = (uint32_t)(remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining);
            size_t bound;
            struct stat information;

            if (chunk->index != index || chunk->offset != index * METADATA_CHUNK_SIZE || chunk->raw_size != expected_size || compression_lz4_bound(chunk->raw_size, &bound) < 0 || chunk->compressed_size == 0U || chunk->compressed_size > bound || chunk_path(storage, &stored->document.id, index, final, chunk_file) < 0 || stat(chunk_file, &information) < 0 || !S_ISREG(information.st_mode) || information.st_size != (off_t)chunk->compressed_size)
            {
                errno = EBADMSG;
                goto error;
            }
        }
        else if (chunk->index != 0U || chunk->offset != 0U || chunk->raw_size != 0U || chunk->compressed_size != 0U)
        {
            errno = EBADMSG;
            goto error;
        }
    }
    trailing = read(fd, scalar, 1U);
    if (trailing != 0)
    {
        if (trailing > 0)
        {
            errno = EBADMSG;
        }
        goto error;
    }
    if (final && verify_document(storage, stored, 1) < 0)
    {
        goto error;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        goto error;
    }
    fd = -1;
    *output = stored;
    return 0;

error:
    if (fd >= 0)
    {
        close(fd);
    }
    if (stored != NULL)
    {
        free(stored->chunks);
        release_document(stored);
    }
    return -1;
}
```

</details>

<a id="fn-storage-c-load-documents"></a>

#### load_documents

Fonte: `storage.c:524` (linha nesta revisão; pode mudar em futuras edições).

```c
static int load_documents(Storage *storage, int final);
```

percorre pending ou objects, lê manifests válidos, confere se o nome da pasta corresponde ao ObjectID e evita duplicatas. Recupera objetos finalizados deixados em VERIFYING após publicação.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `int final`: Seleciona área objects (final) ou pending (pendente); não é retorno da função.

**Funções do projeto usadas diretamente:** [`release_document` (storage.c)](#fn-storage-c-release-document); [`object_hex` (storage.c)](#fn-storage-c-object-hex); [`find_document` (storage.c)](#fn-storage-c-find-document); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`read_manifest` (storage.c)](#fn-storage-c-read-manifest).

**Chamadores diretos encontrados nos fontes inventariados:** [`storage_create` (storage.c)](#fn-storage-c-storage-create).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `closedir(directory)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de load_documents, para acompanhar a explicação</summary>

```c
static int load_documents(Storage *storage, int final)
{
    char path[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    int length;

    length = snprintf(path, sizeof(path), "%s/%s", storage->root, final ? "objects" : "pending");
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    directory = opendir(path);
    if (directory == NULL)
    {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL)
    {
        char manifest[PATH_MAX];
        StoredDocument *stored;

        if (entry->d_name[0] == '.')
        {
            continue;
        }
        length = snprintf(manifest, sizeof(manifest), "%s/%s/manifest.bin", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(manifest))
        {
            continue;
        }
        if (read_manifest(storage, manifest, final, &stored) == 0)
        {
            char expected[OBJECT_ID_HEX_SIZE];

            if (object_hex(&stored->document.id, expected) < 0 || strcmp(entry->d_name, expected) != 0 || find_document(storage, &stored->document.id) != NULL)
            {
                free(stored->chunks);
                release_document(stored);
                continue;
            }
            if (final)
            {
                int recover = stored->state == TRANSFER_VERIFYING;

                stored->state = TRANSFER_FINISHED;
                if (recover && write_manifest(storage, stored, 1) < 0)
                {
                    free(stored->chunks);
                    release_document(stored);
                    (void)closedir(directory);
                    return -1;
                }
            }
            stored->next = storage->documents;
            storage->documents = stored;
        }
    }
    return closedir(directory);
}
```

</details>

<a id="fn-storage-c-storage-create"></a>

#### storage_create

Fonte: `storage.c:586` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_create(const char *root, const NodeID *owner, Storage **output);
```

aloca Storage/mutex, cria as pastas e carrega primeiro os objetos finalizados, depois os pendentes. O owner é o NodeID deste Peer.

**Parâmetros**

- `const char *root`: Diretório raiz do armazenamento persistente.
- `const NodeID *owner`: NodeID do nó proprietário/anunciante, não um PID.
- `Storage **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Funções do projeto usadas diretamente:** [`ensure_directory_tree` (storage.c)](#fn-storage-c-ensure-directory-tree); [`load_documents` (storage.c)](#fn-storage-c-load-documents); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy).

**Chamadores diretos encontrados nos fontes inventariados:** [`initialize_service` (peer_service.c)](#fn-peer-service-c-initialize-service); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_create, para acompanhar a explicação</summary>

```c
int storage_create(const char *root, const NodeID *owner, Storage **output)
{
    Storage *storage;
    char path[PATH_MAX];
    int error;

    if (root == NULL || owner == NULL || output == NULL || strnlen(root, PATH_MAX) >= PATH_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    storage = calloc(1U, sizeof(*storage));
    if (storage == NULL)
    {
        return -1;
    }
    strcpy(storage->root, root);
    storage->owner = *owner;
    if (ensure_directory_tree(storage->root) < 0 || snprintf(path, sizeof(path), "%s/pending", storage->root) < 0 || ensure_directory_tree(path) < 0 || snprintf(path, sizeof(path), "%s/objects", storage->root) < 0 || ensure_directory_tree(path) < 0)
    {
        free(storage);
        return -1;
    }
    error = pthread_mutex_init(&storage->mutex, NULL);
    if (error != 0)
    {
        free(storage);
        errno = error;
        return -1;
    }
    if (load_documents(storage, 1) < 0 || load_documents(storage, 0) < 0)
    {
        storage_destroy(storage);
        return -1;
    }
    *output = storage;
    return 0;
}
```

</details>

<a id="fn-storage-c-storage-destroy"></a>

#### storage_destroy

Fonte: `storage.c:626` (linha nesta revisão; pode mudar em futuras edições).

```c
void storage_destroy(Storage *storage);
```

libera catálogo e mutex; não apaga os arquivos persistidos. É o motivo de documentos poderem ser reencontrados após reinicialização.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.

**Funções do projeto usadas diretamente:** [`release_document` (storage.c)](#fn-storage-c-release-document).

**Chamadores diretos encontrados nos fontes inventariados:** [`peer_service_run` (peer_service.c)](#fn-peer-service-c-peer-service-run); [`storage_create` (storage.c)](#fn-storage-c-storage-create); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de storage_destroy, para acompanhar a explicação</summary>

```c
void storage_destroy(Storage *storage)
{
    StoredDocument *document;

    if (storage == NULL)
    {
        return;
    }
    document = storage->documents;
    while (document != NULL)
    {
        StoredDocument *next = document->next;

        free(document->chunks);
        release_document(document);
        document = next;
    }
    pthread_mutex_destroy(&storage->mutex);
    free(storage);
}
```

</details>

<a id="fn-storage-c-storage-begin"></a>

#### storage_begin

Fonte: `storage.c:647` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_begin(Storage *storage, const TransferDocument *document);
```

abre upload lógico. Se ObjectID e tamanho/contagem já existem, aceita repetição compatível; senão, cria registro CREATED, vetor de chunks e manifest em pending. Não recebe os bytes do arquivo nesta etapa.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Funções do projeto usadas diretamente:** [`content_pdf_name` (content.c)](#fn-content-c-content-pdf-name); [`allocate_document` (storage.c)](#fn-storage-c-allocate-document); [`release_document` (storage.c)](#fn-storage-c-release-document); [`ensure_directory_tree` (storage.c)](#fn-storage-c-ensure-directory-tree); [`document_directory` (storage.c)](#fn-storage-c-document-directory); [`find_document` (storage.c)](#fn-storage-c-find-document); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EEXIST`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_begin, para acompanhar a explicação</summary>

```c
int storage_begin(Storage *storage, const TransferDocument *document)
{
    StoredDocument *stored;
    char directory[PATH_MAX];
    int error;

    if (storage == NULL || document == NULL || !content_pdf_name(document->name) || document->compression != COMPRESSION_LZ4 || document->chunk_count == 0U || document->chunk_count > SIZE_MAX / sizeof(*stored->chunks))
    {
        errno = EINVAL;
        return -1;
    }
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, &document->id);
    if (stored != NULL)
    {
        int matches = stored->document.file_size == document->file_size && stored->document.chunk_count == document->chunk_count;

        pthread_mutex_unlock(&storage->mutex);
        if (!matches)
        {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    stored = allocate_document();
    if (stored == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    stored->chunks = calloc((size_t)document->chunk_count, sizeof(*stored->chunks));
    if (stored->chunks == NULL)
    {
        release_document(stored);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    stored->document = *document;
    stored->owner = storage->owner;
    stored->uploaded_at = (uint64_t)time(NULL);
    stored->state = TRANSFER_CREATED;
    stored->next = storage->documents;
    storage->documents = stored;
    if (document_directory(storage, &document->id, 0, directory) < 0 || ensure_directory_tree(directory) < 0 || write_manifest(storage, stored, 0) < 0)
    {
        storage->documents = stored->next;
        free(stored->chunks);
        release_document(stored);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    pthread_mutex_unlock(&storage->mutex);
    return 0;
}
```

</details>

<a id="fn-storage-c-storage-put-chunk"></a>

#### storage_put_chunk

Fonte: `storage.c:708` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_put_chunk(Storage *storage, const TransferChunk *chunk);
```

Valida índice/offset/tamanhos e hash após LZ4; grava chunk temporário, fsync/rename e manifest. Usa lock por documento. Se persistir o manifest falhar, restaura o descritor/estado anterior em memória para não devolver sucesso falso na repetição.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const TransferChunk *chunk`: Descritor e, quando TransferChunk, ponteiro para conteúdo comprimido.

**Passo a passo**

1. Descomprime e calcula o hash antes de assumir o lock do documento; rejeita bytes incompatíveis com o SHA declarado.
2. Encontra o registro sob lock do catálogo, solta esse lock e adquire o mutex do documento.
3. Calcula offset esperado como índice × 4 MiB e tamanho pelo restante do documento. Rejeita divergência.
4. Se já presente/finalizado, só aceita repetição com descritor compatível; não regrava uma duplicata divergente.
5. Grava o conteúdo comprimido num temporário, faz fsync, fecha e renomeia para o caminho definitivo do chunk.
6. Guarda cópia do descritor/estado anteriores, atualiza-os provisoriamente e persiste o manifest.
7. Se sincronização/manifest falhar, restaura o estado anterior em memória. A existência de bytes no disco não basta para declarar o chunk confirmado.

**Funções do projeto usadas diretamente:** [`compression_lz4_bound` (compression.c)](#fn-compression-c-compression-lz4-bound); [`compression_lz4_decompress` (compression.c)](#fn-compression-c-compression-lz4-decompress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`sync_parent` (storage.c)](#fn-storage-c-sync-parent); [`write_all_fd` (storage.c)](#fn-storage-c-write-all-fd); [`chunk_path` (storage.c)](#fn-storage-c-chunk-path); [`find_document` (storage.c)](#fn-storage-c-find-document); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EBADMSG`, `EEXIST`, `ENAMETOOLONG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_put_chunk, para acompanhar a explicação</summary>

```c
int storage_put_chunk(Storage *storage, const TransferChunk *chunk)
{
    StoredDocument *stored;
    StoredChunk *record;
    uint8_t calculated[OBJECT_ID_SIZE];
    uint8_t *raw = NULL;
    char final_path[PATH_MAX];
    char temporary[PATH_MAX];
    uint64_t expected_offset;
    uint64_t remaining;
    uint32_t expected_size;
    size_t bound;
    int fd = -1;
    int error;
    int length;

    if (storage == NULL || chunk == NULL || chunk->data == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (compression_lz4_bound(chunk->raw_size, &bound) < 0 || chunk->compressed_size > bound || compression_lz4_decompress(chunk->data, chunk->compressed_size, chunk->raw_size, &raw) < 0 || content_sha256(raw, chunk->raw_size, calculated) < 0 || memcmp(calculated, chunk->hash, OBJECT_ID_SIZE) != 0)
    {
        free(raw);
        errno = EBADMSG;
        return -1;
    }
    free(raw);
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, &chunk->id);
    if (stored == NULL || chunk->index >= stored->document.chunk_count || chunk->index > UINT64_MAX / METADATA_CHUNK_SIZE)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = stored == NULL ? ENOENT : EINVAL;
        return -1;
    }
    /* Registros não são removidos enquanto o serviço está ativo. */
    pthread_mutex_unlock(&storage->mutex);
    error = pthread_mutex_lock(&stored->mutex);
    if (error != 0) { errno = error; return -1; }
    expected_offset = chunk->index * METADATA_CHUNK_SIZE;
    remaining = stored->document.file_size - expected_offset;
    expected_size = (uint32_t)(remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining);
    if (chunk->offset != expected_offset || chunk->raw_size != expected_size)
    {
        pthread_mutex_unlock(&stored->mutex);
        errno = EINVAL;
        return -1;
    }
    record = &stored->chunks[chunk->index];
    if (stored->state == TRANSFER_FINISHED)
    {
        int matches = record->present && record->raw_size == chunk->raw_size && record->compressed_size == chunk->compressed_size && memcmp(record->hash, chunk->hash, OBJECT_ID_SIZE) == 0;

        pthread_mutex_unlock(&stored->mutex);
        if (!matches)
        {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    if (record->present)
    {
        int matches = record->raw_size == chunk->raw_size && record->compressed_size == chunk->compressed_size && memcmp(record->hash, chunk->hash, OBJECT_ID_SIZE) == 0;

        pthread_mutex_unlock(&stored->mutex);
        if (!matches)
        {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    if (chunk_path(storage, &chunk->id, chunk->index, 0, final_path) < 0)
    {
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    length = snprintf(temporary, sizeof(temporary), "%s.tmp-%ld", final_path, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary))
    {
        pthread_mutex_unlock(&stored->mutex);
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write_all_fd(fd, chunk->data, chunk->compressed_size) < 0 || fsync(fd) < 0)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        unlink(temporary);
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        unlink(temporary);
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    fd = -1;
    if (rename(temporary, final_path) < 0)
    {
        unlink(temporary);
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    StoredChunk previous_record = *record;
    TransferState previous_state = stored->state;
    record->index = chunk->index;
    record->offset = chunk->offset;
    record->raw_size = chunk->raw_size;
    record->compressed_size = chunk->compressed_size;
    memcpy(record->hash, chunk->hash, OBJECT_ID_SIZE);
    record->present = 1;
    stored->state = TRANSFER_TRANSFERRING;
    if (sync_parent(final_path) < 0 || write_manifest(storage, stored, 0) < 0)
    {
        *record = previous_record;
        stored->state = previous_state;
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    pthread_mutex_unlock(&stored->mutex);
    return 0;
}
```

</details>

<a id="fn-storage-c-verify-document"></a>

#### verify_document

Fonte: `storage.c:844` (linha nesta revisão; pode mudar em futuras edições).

```c
static int verify_document(Storage *storage, StoredDocument *stored, int final);
```

Reabre e descomprime chunks em ordem, verifica hashes individuais e alimenta EVP_DigestUpdate. Ao final compara tamanho e ObjectID. Não cria cópia temporária integral do documento.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `StoredDocument *stored`: Registro interno do armazenamento; contém estado, descritores e mutex por documento.
- `int final`: Seleciona área objects (final) ou pending (pendente); não é retorno da função.

**Funções do projeto usadas diretamente:** [`compression_lz4_decompress` (compression.c)](#fn-compression-c-compression-lz4-decompress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`read_all_fd` (storage.c)](#fn-storage-c-read-all-fd); [`chunk_path` (storage.c)](#fn-storage-c-chunk-path).

**Chamadores diretos encontrados nos fontes inventariados:** [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `open`: Obtém descritor para um caminho. Flags como O_EXCL, O_NOFOLLOW e O_TRUNC têm efeitos diferentes; confira as flags concretas no código.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `EVP_DigestUpdate`: Acrescenta bytes ao SHA-256 incremental, mantendo apenas o estado do hash e o bloco corrente.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EIO`, `ENODATA`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de verify_document, para acompanhar a explicação</summary>

```c
static int verify_document(Storage *storage, StoredDocument *stored, int final)
{
    ObjectID actual;
    uint64_t actual_size = 0U;
    uint64_t index;
    unsigned int digest_size = 0U;
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    if (hash == NULL) return -1;
    if (EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1) { EVP_MD_CTX_free(hash); errno = EIO; return -1; }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        StoredChunk *record = &stored->chunks[index];
        char path[PATH_MAX];
        uint8_t *compressed = NULL;
        uint8_t *raw = NULL;
        uint8_t digest[OBJECT_ID_SIZE];
        int chunk_fd = -1;

        if (!record->present || chunk_path(storage, &stored->document.id, index, final, path) < 0)
        {
            errno = ENODATA;
            goto error;
        }
        compressed = malloc(record->compressed_size);
        if (compressed == NULL)
        {
            goto error;
        }
        chunk_fd = open(path, O_RDONLY);
        if (chunk_fd < 0 || read_all_fd(chunk_fd, compressed, record->compressed_size) < 0)
        {
            if (chunk_fd >= 0)
            {
                close(chunk_fd);
            }
            free(compressed);
            goto error;
        }
        if (close(chunk_fd) < 0)
        {
            chunk_fd = -1;
            free(compressed);
            goto error;
        }
        chunk_fd = -1;
        if (compression_lz4_decompress(compressed, record->compressed_size, record->raw_size, &raw) < 0 || content_sha256(raw, record->raw_size, digest) < 0 || memcmp(digest, record->hash, OBJECT_ID_SIZE) != 0 || EVP_DigestUpdate(hash, raw, record->raw_size) != 1)
        {
            free(compressed);
            free(raw);
            errno = EBADMSG;
            goto error;
        }
        actual_size += record->raw_size;
        free(compressed);
        free(raw);
    }
    if (EVP_DigestFinal_ex(hash, actual.bytes, &digest_size) != 1 || digest_size != OBJECT_ID_SIZE || actual_size != stored->document.file_size || memcmp(actual.bytes, stored->document.id.bytes, OBJECT_ID_SIZE) != 0)
    {
        errno = EBADMSG;
        goto error;
    }
    EVP_MD_CTX_free(hash);
    return 0;
error:
    EVP_MD_CTX_free(hash);
    return -1;
}
```

</details>

<a id="fn-storage-c-storage-commit"></a>

#### storage_commit

Fonte: `storage.c:912` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_commit(Storage *storage, const ObjectID *id, TransferDocument *document);
```

Exige todos os chunks e verifica integridade sob lock do documento. Grava manifest final em pending, renomeia diretório e sincroniza pais antes de confirmar FINISHED em memória. Falha mantém erro; repetição compatível é aceita.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Passo a passo**

1. Obtém o registro por ID e trava o documento. Um documento já FINISHED devolve seus metadados sem repetir publicação.
2. Exige present em todos os chunks e muda para VERIFYING.
3. verify_document descomprime em ordem, valida cada hash e recalcula o SHA do documento completo incrementalmente.
4. Escreve o manifest final ainda em pending; rename publica o diretório em objects.
5. Sincroniza os diretórios pais. Se isso falhar, tenta reverter o rename e retorna erro; não garante reversão caso o próprio sistema de arquivos recuse.
6. Somente após sucesso marca FINISHED em memória e copia os metadados de saída. Anunciar ao Super Peer não é tarefa desta função.

**Funções do projeto usadas diretamente:** [`sync_parent` (storage.c)](#fn-storage-c-sync-parent); [`document_directory` (storage.c)](#fn-storage-c-document-directory); [`find_document` (storage.c)](#fn-storage-c-find-document); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`verify_document` (storage.c)](#fn-storage-c-verify-document).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store); [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.
- `rename`: Muda o nome/localização de uma entrada no sistema de arquivos. A atomicidade dessa mudança não engloba outras operações de rede.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`, `ENODATA`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_commit, para acompanhar a explicação</summary>

```c
int storage_commit(Storage *storage, const ObjectID *id, TransferDocument *document)
{
    StoredDocument *stored;
    char pending[PATH_MAX];
    char final[PATH_MAX];
    uint64_t index;
    int error;

    if (storage == NULL || id == NULL || document == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, id);
    if (stored == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENOENT;
        return -1;
    }
    pthread_mutex_unlock(&storage->mutex);
    error = pthread_mutex_lock(&stored->mutex);
    if (error != 0) { errno = error; return -1; }
    if (stored->state == TRANSFER_FINISHED)
    {
        *document = stored->document;
        pthread_mutex_unlock(&stored->mutex);
        return 0;
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        if (!stored->chunks[index].present)
        {
            pthread_mutex_unlock(&stored->mutex);
            errno = ENODATA;
            return -1;
        }
    }
    stored->state = TRANSFER_VERIFYING;
    if (verify_document(storage, stored, 0) < 0 || document_directory(storage, id, 0, pending) < 0 || document_directory(storage, id, 1, final) < 0)
    {
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    /* O manifest final é escrito em pending antes da publicação do diretório. */
    if (write_manifest(storage, stored, 0) < 0 || rename(pending, final) < 0)
    {
        stored->state = TRANSFER_VERIFYING;
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    if (sync_parent(pending) < 0 || sync_parent(final) < 0)
    {
        /* Reverte para permitir nova tentativa sem confirmar estado não durável. */
        if (rename(final, pending) == 0) stored->state = TRANSFER_VERIFYING;
        pthread_mutex_unlock(&stored->mutex);
        return -1;
    }
    stored->state = TRANSFER_FINISHED;
    *document = stored->document;
    pthread_mutex_unlock(&stored->mutex);
    return 0;
}
```

</details>

<a id="fn-storage-c-storage-find"></a>

#### storage_find

Fonte: `storage.c:982` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_find(Storage *storage, TransferSelectorType type, const ObjectID *id, const char *name, TransferDocument *document);
```

consulta apenas documentos FINISHED por ID ou nome; nome ambíguo para IDs diferentes gera ENOTUNIQ.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `TransferSelectorType type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `const char *name`: Nome textual usado como chave, metadado ou variável de ambiente, conforme a finalidade descrita.
- `TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_storage_atomic.c)](#fn-tests-c2-test-storage-atomic-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOTUNIQ`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_find, para acompanhar a explicação</summary>

```c
int storage_find(Storage *storage, TransferSelectorType type, const ObjectID *id, const char *name, TransferDocument *document)
{
    StoredDocument *current;
    StoredDocument *found = NULL;
    int error;

    if (storage == NULL || document == NULL || (type == TRANSFER_SELECTOR_OBJECT_ID && id == NULL) || (type == TRANSFER_SELECTOR_NAME && name == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    for (current = storage->documents; current != NULL; current = current->next)
    {
        int matches = type == TRANSFER_SELECTOR_OBJECT_ID ? memcmp(current->document.id.bytes, id->bytes, OBJECT_ID_SIZE) == 0 : strcmp(current->document.name, name) == 0;

        if (matches && current->state == TRANSFER_FINISHED)
        {
            if (found != NULL && memcmp(found->document.id.bytes, current->document.id.bytes, OBJECT_ID_SIZE) != 0)
            {
                pthread_mutex_unlock(&storage->mutex);
                errno = ENOTUNIQ;
                return -1;
            }
            found = current;
        }
    }
    if (found == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENOENT;
        return -1;
    }
    *document = found->document;
    pthread_mutex_unlock(&storage->mutex);
    return 0;
}
```

</details>

<a id="fn-storage-c-storage-read-chunk"></a>

#### storage_read_chunk

Fonte: `storage.c:1025` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_read_chunk(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data);
```

lê chunk comprimido de objeto finalizado e devolve descritor + buffer de bytes. O chamador é dono de owned_data e deve usar free; chunk.data aponta para o mesmo buffer.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.
- `TransferChunk *chunk`: Descritor e, quando TransferChunk, ponteiro para conteúdo comprimido.
- `uint8_t **owned_data`: Ponteiro de saída para o buffer alocado dos bytes comprimidos; o chamador usa free.

**Funções do projeto usadas diretamente:** [`read_all_fd` (storage.c)](#fn-storage-c-read-all-fd); [`chunk_path` (storage.c)](#fn-storage-c-chunk-path); [`find_document` (storage.c)](#fn-storage-c-find-document).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_download` (peer_service.c)](#fn-peer-service-c-handle-download).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_read_chunk, para acompanhar a explicação</summary>

```c
int storage_read_chunk(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data)
{
    StoredDocument *stored;
    StoredChunk record;
    char path[PATH_MAX];
    uint8_t *data;
    int fd;
    int error;

    if (storage == NULL || id == NULL || chunk == NULL || owned_data == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *owned_data = NULL;
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, id);
    if (stored == NULL || stored->state != TRANSFER_FINISHED || index >= stored->document.chunk_count)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENOENT;
        return -1;
    }
    record = stored->chunks[index];
    if (chunk_path(storage, id, index, 1, path) < 0)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    pthread_mutex_unlock(&storage->mutex);
    data = malloc(record.compressed_size);
    if (data == NULL)
    {
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || read_all_fd(fd, data, record.compressed_size) < 0)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        free(data);
        return -1;
    }
    if (close(fd) < 0)
    {
        free(data);
        return -1;
    }
    memset(chunk, 0, sizeof(*chunk));
    chunk->id = *id;
    chunk->index = record.index;
    chunk->offset = record.offset;
    chunk->raw_size = record.raw_size;
    chunk->compressed_size = record.compressed_size;
    memcpy(chunk->hash, record.hash, OBJECT_ID_SIZE);
    chunk->data = data;
    *owned_data = data;
    return 0;
}
```

</details>

<a id="fn-storage-c-storage-list"></a>

#### storage_list

Fonte: `storage.c:1092` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_list(Storage *storage, TransferDocument **documents, size_t *count);
```

copia os documentos FINISHED do catálogo. O Peer usa a lista ao reiniciar para reanunciá-los; o chamador libera a matriz com free.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `TransferDocument **documents`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.
- `size_t *count`: Quantidade de elementos ou ponteiro para devolvê-la; não necessariamente bytes.

**Chamadores diretos encontrados nos fontes inventariados:** [`announce_catalog` (peer_service.c)](#fn-peer-service-c-announce-catalog).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_list, para acompanhar a explicação</summary>

```c
int storage_list(Storage *storage, TransferDocument **documents, size_t *count)
{
    StoredDocument *current;
    TransferDocument *list;
    size_t total = 0U;
    size_t index = 0U;
    int error;

    if (storage == NULL || documents == NULL || count == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *documents = NULL;
    *count = 0U;
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    for (current = storage->documents; current != NULL; current = current->next)
    {
        if (current->state == TRANSFER_FINISHED)
        {
            ++total;
        }
    }
    list = total == 0U ? NULL : malloc(total * sizeof(*list));
    if (total != 0U && list == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    if (total == 0U)
    {
        pthread_mutex_unlock(&storage->mutex);
        return 0;
    }
    for (current = storage->documents; current != NULL; current = current->next)
    {
        if (current->state == TRANSFER_FINISHED)
        {
            list[index++] = current->document;
        }
    }
    pthread_mutex_unlock(&storage->mutex);
    *documents = list;
    *count = total;
    return 0;
}
```

</details>

<a id="fn-storage-c-storage-descriptors"></a>

#### storage_descriptors

Fonte: `storage.c:1144` (linha nesta revisão; pode mudar em futuras edições).

```c
int storage_descriptors(Storage *storage, const ObjectID *id, MetadataChunk **output);
```

consulta catálogo, adquire lock do documento e devolve cópia dos descritores somente se FINISHED. Essa cópia é usada no anúncio, não inclui conteúdo do PDF.

**Parâmetros**

- `Storage *storage`: Instância do catálogo/armazenamento local, com seus locks e diretório raiz.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `MetadataChunk **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.

**Funções do projeto usadas diretamente:** [`find_document` (storage.c)](#fn-storage-c-find-document).

**Chamadores diretos encontrados nos fontes inventariados:** [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `pthread_mutex_lock`: Inicia seção exclusiva sobre a estrutura indicada. Não significa que toda a função está protegida; acompanhe cada unlock.
- `pthread_mutex_unlock`: Termina uma seção crítica. Recursos internos não devem ser usados depois de sua vida útil acabar.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `ENOENT`, `ENODATA`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de storage_descriptors, para acompanhar a explicação</summary>

```c
int storage_descriptors(Storage *storage, const ObjectID *id, MetadataChunk **output)
{
    if (storage == NULL || id == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    pthread_mutex_lock(&storage->mutex);
    StoredDocument *stored = find_document(storage, id);
    if (stored == NULL) { pthread_mutex_unlock(&storage->mutex); errno = ENOENT; return -1; }
    pthread_mutex_unlock(&storage->mutex);
    pthread_mutex_lock(&stored->mutex);
    if (stored->state != TRANSFER_FINISHED) { pthread_mutex_unlock(&stored->mutex); errno = ENODATA; return -1; }
    MetadataChunk *list = calloc((size_t)stored->document.chunk_count, sizeof(*list));
    if (list == NULL) { pthread_mutex_unlock(&stored->mutex); return -1; }
    for (uint64_t i = 0U; i < stored->document.chunk_count; ++i)
    {
        list[i].index = i;
        list[i].offset = stored->chunks[i].offset;
        list[i].raw_size = stored->chunks[i].raw_size;
        list[i].compressed_size = stored->chunks[i].compressed_size;
        memcpy(list[i].hash, stored->chunks[i].hash, OBJECT_ID_SIZE);
    }
    pthread_mutex_unlock(&stored->mutex);
    *output = list;
    return 0;
}
```

</details>

<a id="mod-superpeer-c"></a>

### superpeer.c

[main](#fn-superpeer-c-main)

<a id="fn-superpeer-c-main"></a>

#### main

Fonte: `superpeer.c:4` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(int argc, char **argv);
```

É o ponto de entrada exclusivo do executável Super Peer. Recebe argc/argv do sistema, delega integralmente a superpeer_run e devolve o mesmo código de saída. Não inicializa outra identidade e não depende de macro para escolher seu papel.

**Parâmetros**

- `int argc`: Quantidade de argumentos; quando ponteiro, a função pode atualizar a contagem após retirar opções.
- `char **argv`: Vetor de argumentos terminados em NUL; as rotinas de configuração/CLI podem reorganizar seus ponteiros.

**Funções do projeto usadas diretamente:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Retorno:** as expressões presentes nesta função são `superpeer_run(argc, argv)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(int argc, char **argv)
{
    return superpeer_run(argc, argv);
}
```

</details>

<a id="mod-superpeer-app-c"></a>

### superpeer_app.c

[handle_signal](#fn-superpeer-app-c-handle-signal) · [parse_port](#fn-superpeer-app-c-parse-port) · [parse_command_type](#fn-superpeer-app-c-parse-command-type) · [parse_node_arguments](#fn-superpeer-app-c-parse-node-arguments) · [print_node_id](#fn-superpeer-app-c-print-node-id) · [message_type_name](#fn-superpeer-app-c-message-type-name) · [set_text_payload](#fn-superpeer-app-c-set-text-payload) · [message_payload_equals](#fn-superpeer-app-c-message-payload-equals) · [node_id_is_zero](#fn-superpeer-app-c-node-id-is-zero) · [encode_join_payload_alloc](#fn-superpeer-app-c-encode-join-payload-alloc) · [initialize_local_identity](#fn-superpeer-app-c-initialize-local-identity) · [send_reply](#fn-superpeer-app-c-send-reply) · [register_join](#fn-superpeer-app-c-register-join) · [register_announcement](#fn-superpeer-app-c-register-announcement) · [answer_lookup](#fn-superpeer-app-c-answer-lookup) · [handle_client](#fn-superpeer-app-c-handle-client) · [accept_clients](#fn-superpeer-app-c-accept-clients) · [connect_and_join](#fn-superpeer-app-c-connect-and-join) · [execute_command](#fn-superpeer-app-c-execute-command) · [print_usage](#fn-superpeer-app-c-print-usage) · [superpeer_run](#fn-superpeer-app-c-superpeer-run)

<a id="fn-superpeer-app-c-handle-signal"></a>

#### handle_signal

Fonte: `superpeer_app.c:58` (linha nesta revisão; pode mudar em futuras edições).

```c
static void handle_signal(int signal_number);
```

limpa a flag sig_atomic_t que mantém o processo vivo; a saída normal para o runtime é feita depois, fora do handler.

**Parâmetros**

- `int signal_number`: Sinal recebido pelo handler; não representa erro de rede.

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de handle_signal, para acompanhar a explicação</summary>

```c
static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}
```

</details>

<a id="fn-superpeer-app-c-parse-port"></a>

#### parse_port

Fonte: `superpeer_app.c:65` (linha nesta revisão; pode mudar em futuras edições).

```c
static int parse_port(const char *text, uint16_t *port);
```

valida porta decimal no intervalo permitido.

**Parâmetros**

- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.
- `uint16_t *port`: Porta; se ponteiro, recebe a conversão validada do texto.

**Chamadores diretos encontrados nos fontes inventariados:** [`parse_node_arguments` (superpeer_app.c)](#fn-superpeer-app-c-parse-node-arguments).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `strtoul`: Converte texto decimal e informa onde a conversão terminou; a validação adicional restringe a faixa permitida.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de parse_port, para acompanhar a explicação</summary>

```c
static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || port == NULL || *text == '\0')
    {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > UINT16_MAX)
    {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}
```

</details>

<a id="fn-superpeer-app-c-parse-command-type"></a>

#### parse_command_type

Fonte: `superpeer_app.c:87` (linha nesta revisão; pode mudar em futuras edições).

```c
static int parse_command_type(const char *text, Message_Type *type);
```

converte ping, join ou leave para o código de mensagem correspondente.

**Parâmetros**

- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.
- `Message_Type *type`: Código de mensagem/seletor, de acordo com o enum da assinatura.

**Chamadores diretos encontrados nos fontes inventariados:** [`parse_node_arguments` (superpeer_app.c)](#fn-superpeer-app-c-parse-node-arguments).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de parse_command_type, para acompanhar a explicação</summary>

```c
static int parse_command_type(const char *text, Message_Type *type)
{
    if (text == NULL || type == NULL)
    {
        return -1;
    }
    if (strcmp(text, "ping") == 0)
    {
        *type = M_PING;
        return 0;
    }
    if (strcmp(text, "join") == 0)
    {
        *type = M_JOIN;
        return 0;
    }
    if (strcmp(text, "leave") == 0)
    {
        *type = M_LEAVE;
        return 0;
    }
    return -1;
}
```

</details>

<a id="fn-superpeer-app-c-parse-node-arguments"></a>

#### parse_node_arguments

Fonte: `superpeer_app.c:112` (linha nesta revisão; pode mudar em futuras edições).

```c
static int parse_node_arguments(int argc, char **argv, NodeArguments *arguments);
```

aceita forma posicional legada e opções getopt_long. Distingue servidor e comando; app_config_load já interpretou o arquivo e as opções comuns antes dessa chamada.

**Parâmetros**

- `int argc`: Quantidade de argumentos; quando ponteiro, a função pode atualizar a contagem após retirar opções.
- `char **argv`: Vetor de argumentos terminados em NUL; as rotinas de configuração/CLI podem reorganizar seus ponteiros.
- `NodeArguments *arguments`: Estrutura com argumentos já interpretados.

**Funções do projeto usadas diretamente:** [`parse_port` (superpeer_app.c)](#fn-superpeer-app-c-parse-port); [`parse_command_type` (superpeer_app.c)](#fn-superpeer-app-c-parse-command-type).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de parse_node_arguments, para acompanhar a explicação</summary>

```c
static int parse_node_arguments(int argc, char **argv, NodeArguments *arguments)
{
    static const struct option long_options[] = {
        {"cmd", required_argument, NULL, 'c'},
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {"config", required_argument, NULL, 'f'},
        {"name", required_argument, NULL, 'n'},
        {NULL, 0, NULL, 0}
    };
    int option;
    int have_command = 0;
    int have_host = 0;
    int have_port = 1;
    int have_name = 0;

    if (arguments == NULL || argc < 1)
    {
        return -1;
    }

    memset(arguments, 0, sizeof(*arguments));
    arguments->node_name = "peer";
    arguments->local_port = app_config.port;
    if (argc == 1) return 0;

    /* Preserva a interface posicional original: peer <porta> [<ip> <porta>]. */
    if (argv[1][0] != '-')
    {
        if (argc != 2 && argc != 4)
        {
            return -1;
        }
        if (parse_port(argv[1], &arguments->local_port) < 0)
        {
            return -1;
        }
        have_port = 1;
        if (argc == 4)
        {
            arguments->remote_ip = argv[2];
            if (parse_port(argv[3], &arguments->remote_port) < 0)
            {
                return -1;
            }
        }
        return 0;
    }

    opterr = 0;
    optind = 1;
    while ((option = getopt_long(argc, argv, "c:h:p:f:n:", long_options, NULL)) != -1)
    {
        switch (option)
        {
        case 'c':
            if (parse_command_type(optarg, &arguments->command_type) < 0)
            {
                return -1;
            }
            have_command = 1;
            break;
        case 'h':
            arguments->remote_ip = optarg;
            have_host = optarg[0] != '\0';
            break;
        case 'p':
            if (parse_port(optarg, &arguments->local_port) < 0)
            {
                return -1;
            }
            have_port = 1;
            break;
        case 'f':
            arguments->config_path = optarg;
            break;
        case 'n':
            arguments->node_name = optarg;
            if (arguments->node_name[0] == '\0')
            {
                return -1;
            }
            have_name = 1;
            break;
        default:
            return -1;
        }
    }

    if (optind != argc || !have_port)
    {
        return -1;
    }

    if (have_command)
    {
        if (!have_host || arguments->config_path != NULL || have_name)
        {
            return -1;
        }
        arguments->command_mode = 1;
        arguments->remote_port = arguments->local_port;
        arguments->local_port = 0U;
        return 0;
    }

    if (have_host)
    {
        return -1;
    }
    if (arguments->config_path != NULL && access(arguments->config_path, R_OK) != 0)
    {
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-superpeer-app-c-print-node-id"></a>

#### print_node_id

Fonte: `superpeer_app.c:230` (linha nesta revisão; pode mudar em futuras edições).

```c
static void print_node_id(const uint8_t node_id[NODE_ID_SIZE]);
```

imprime os 32 bytes como 64 dígitos hexadecimais.

**Parâmetros**

- `const uint8_t node_id[NODE_ID_SIZE]`: Identificador binário de nó, com 32 bytes.

**Chamadores diretos encontrados nos fontes inventariados:** [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de print_node_id, para acompanhar a explicação</summary>

```c
static void print_node_id(const uint8_t node_id[NODE_ID_SIZE])
{
    size_t index;

    for (index = 0U; index < NODE_ID_SIZE; ++index)
    {
        printf("%02x", (unsigned)node_id[index]);
    }
}
```

</details>

<a id="fn-superpeer-app-c-message-type-name"></a>

#### message_type_name

Fonte: `superpeer_app.c:241` (linha nesta revisão; pode mudar em futuras edições).

```c
static const char *message_type_name(Message_Type type);
```

devolve nome textual para logs TX/RX dos comandos C1.

**Parâmetros**

- `Message_Type type`: Código de mensagem/seletor, de acordo com o enum da assinatura.

**Chamadores diretos encontrados nos fontes inventariados:** [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command).

**Retorno:** as expressões presentes nesta função são ``. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de message_type_name, para acompanhar a explicação</summary>

```c
static const char *message_type_name(Message_Type type)
{
    switch (type)
    {
    case M_JOIN:
        return "JOIN";
    case M_ACK:
        return "ACK";
    case M_ERROR:
        return "ERROR";
    case M_PING:
        return "PING";
    case M_PONG:
        return "PONG";
    case M_LEAVE:
        return "LEAVE";
    default:
        return "UNKNOWN";
    }
}
```

</details>

<a id="fn-superpeer-app-c-set-text-payload"></a>

#### set_text_payload

Fonte: `superpeer_app.c:263` (linha nesta revisão; pode mudar em futuras edições).

```c
static int set_text_payload(Message *message, const char *text);
```

aloca e copia payload textual sem o NUL final; usado para PING. A mensagem passa a ser dona do buffer.

**Parâmetros**

- `Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.
- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.

**Chamadores diretos encontrados nos fontes inventariados:** [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de set_text_payload, para acompanhar a explicação</summary>

```c
static int set_text_payload(Message *message, const char *text)
{
    size_t text_size;

    if (message == NULL || text == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    text_size = strlen(text);
    if (text_size == 0U || text_size > UINT32_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    message->payload = malloc(text_size);
    if (message->payload == NULL)
    {
        return -1;
    }
    memcpy(message->payload, text, text_size);
    message->header.payload_size = (uint32_t)text_size;
    return 0;
}
```

</details>

<a id="fn-superpeer-app-c-message-payload-equals"></a>

#### message_payload_equals

Fonte: `superpeer_app.c:291` (linha nesta revisão; pode mudar em futuras edições).

```c
static int message_payload_equals(const Message *message, const char *text);
```

compara comprimento e bytes de um payload com texto esperado, sem exigir terminador NUL na rede.

**Parâmetros**

- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.
- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `0`, `message->header.payload_size == text_size && message->payload != NULL && memcmp(message->payload, text, text_size) == 0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de message_payload_equals, para acompanhar a explicação</summary>

```c
static int message_payload_equals(const Message *message, const char *text)
{
    size_t text_size;

    if (message == NULL || text == NULL)
    {
        return 0;
    }

    text_size = strlen(text);
    return message->header.payload_size == text_size && message->payload != NULL && memcmp(message->payload, text, text_size) == 0;
}
```

</details>

<a id="fn-superpeer-app-c-node-id-is-zero"></a>

#### node_id_is_zero

Fonte: `superpeer_app.c:305` (linha nesta revisão; pode mudar em futuras edições).

```c
static int node_id_is_zero(const uint8_t node_id[NODE_ID_SIZE]);
```

verifica se os 32 bytes são zero. JOIN inicial pode não conhecer o ID do destinatário e usar destino zerado.

**Parâmetros**

- `const uint8_t node_id[NODE_ID_SIZE]`: Identificador binário de nó, com 32 bytes.

**Chamadores diretos encontrados nos fontes inventariados:** [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client).

**Retorno:** as expressões presentes nesta função são `0`, `1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de node_id_is_zero, para acompanhar a explicação</summary>

```c
static int node_id_is_zero(const uint8_t node_id[NODE_ID_SIZE])
{
    size_t index;

    for (index = 0U; index < NODE_ID_SIZE; ++index)
    {
        if (node_id[index] != 0U)
        {
            return 0;
        }
    }
    return 1;
}
```

</details>

<a id="fn-superpeer-app-c-encode-join-payload-alloc"></a>

#### encode_join_payload_alloc

Fonte: `superpeer_app.c:332` (linha nesta revisão; pode mudar em futuras edições).

```c
static int encode_join_payload_alloc(const NodeConfig *config, uint8_t **payload_output);
```

Aloca 64 bytes e delega serialização a rpc_encode_join_payload; libera em erro. Evita outro encoder JOIN.

**Parâmetros**

- `const NodeConfig *config`: Estrutura de configuração recebida ou preenchida pela rotina.
- `uint8_t **payload_output`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.

**Funções do projeto usadas diretamente:** [`rpc_encode_join_payload` (rpc.c)](#fn-rpc-c-rpc-encode-join-payload).

**Chamadores diretos encontrados nos fontes inventariados:** [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de encode_join_payload_alloc, para acompanhar a explicação</summary>

```c
static int encode_join_payload_alloc(const NodeConfig *config, uint8_t **payload_output)
{
    uint8_t *payload = malloc(JOIN_PAYLOAD_WIRE_SIZE);
    if (payload == NULL) return -1;
    if (rpc_encode_join_payload(config, payload) < 0) { free(payload); return -1; }
    *payload_output = payload;
    return 0;
}
```

</details>

<a id="fn-superpeer-app-c-initialize-local-identity"></a>

#### initialize_local_identity

Fonte: `superpeer_app.c:344` (linha nesta revisão; pode mudar em futuras edições).

```c
static int initialize_local_identity(PeerContext *peer, uint16_t local_port);
```

cria Node local e SuperPeer membership com **o mesmo UUID**, garantindo que os dois objetos tenham o mesmo NodeID.

**Parâmetros**

- `PeerContext *peer`: Contexto local do Super Peer utilizado pelo handler.
- `uint16_t local_port`: Porta do serviço local e de identificação de seu socket Unix.

**Funções do projeto usadas diretamente:** [`app_identity` (app_config.c)](#fn-app-config-c-app-identity); [`superpeer_config_init_with_uuid` (membership.c)](#fn-membership-c-superpeer-config-init-with-uuid); [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`node_init` (node.c)](#fn-node-c-node-init).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de initialize_local_identity, para acompanhar a explicação</summary>

```c
static int initialize_local_identity(PeerContext *peer, uint16_t local_port)
{
    NodeConfig config;
    SuperPeerConfig superpeer_config;

    char directory[512];
    (void)snprintf(directory, sizeof(directory), ".superpeer_storage/%u", (unsigned)local_port);
    if (app_config.data_dir[0] != '\0') strcpy(directory, app_config.data_dir);
    if (app_identity(directory, &config, local_port) < 0 || node_init(&peer->local_node, &config) < 0)
    {
        return -1;
    }

    /* Reutiliza o UUID para que Node e SuperPeer locais tenham o mesmo ID. */
    if (superpeer_config_init_with_uuid(&superpeer_config, config.ip, config.port, config.uuid) < 0 || superpeer_create(&superpeer_config, &peer->superpeer) < 0)
    {
        return -1;
    }

    return 0;
}
```

</details>

<a id="fn-superpeer-app-c-send-reply"></a>

#### send_reply

Fonte: `superpeer_app.c:367` (linha nesta revisão; pode mudar em futuras edições).

```c
static int send_reply(const PeerContext *peer, int client_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size, int include_node_descriptor);
```

constrói resposta com NodeID local, destino da requisição, mesmo TransactionID e payload opcional; pode anexar o próprio descritor de nó em ACK de JOIN.

**Parâmetros**

- `const PeerContext *peer`: Contexto local do Super Peer utilizado pelo handler.
- `int client_fd`: Descritor conectado, diferente do listener.
- `const Message *request`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.
- `Message_Type type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `uint32_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `int include_node_descriptor`: Determina se a resposta de JOIN inclui o descritor de identidade local.

**Funções do projeto usadas diretamente:** [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`remote_error_encode` (remote_error.h)](#fn-remote-error-h-remote-error-encode); [`encode_join_payload_alloc` (superpeer_app.c)](#fn-superpeer-app-c-encode-join-payload-alloc).

**Chamadores diretos encontrados nos fontes inventariados:** [`answer_lookup` (superpeer_app.c)](#fn-superpeer-app-c-answer-lookup); [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `result`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de send_reply, para acompanhar a explicação</summary>

```c
static int send_reply(const PeerContext *peer, int client_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size, int include_node_descriptor)
{
    Message reply;
    int result;
    uint8_t error_payload[2];

    if (type == M_ERROR) { remote_error_encode(errno, error_payload); payload = error_payload; payload_size = sizeof(error_payload); }

    if (peer == NULL || request == NULL)
    {
        return -1;
    }

    if (message_init(&reply) < 0)
    {
        return -1;
    }
    reply.header.message_type = (uint8_t)type;
    memcpy(reply.header.source_node, peer->local_node.id.bytes, NODE_ID_SIZE);
    memcpy(reply.header.destination_node, request->header.source_node, NODE_ID_SIZE);
    memcpy(reply.header.transaction_id, request->header.transaction_id, TRANSACTION_ID_SIZE);
    reply.header.timestamp = (uint64_t)time(NULL);

    if (include_node_descriptor != 0)
    {
        reply.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;
        if (encode_join_payload_alloc(&peer->local_node.config, &reply.payload) < 0)
        {
            message_free(&reply);
            return -1;
        }
    }
    else if (payload_size > 0U)
    {
        if (payload == NULL)
        {
            message_free(&reply);
            errno = EINVAL;
            return -1;
        }
        reply.payload = malloc(payload_size);
        if (reply.payload == NULL)
        {
            message_free(&reply);
            return -1;
        }
        memcpy(reply.payload, payload, payload_size);
        reply.header.payload_size = payload_size;
    }
    else
    {
        reply.header.payload_size = 0U;
        reply.payload = NULL;
    }

    result = protocol_send_message(client_fd, &reply);
    message_free(&reply);
    return result;
}
```

</details>

<a id="fn-superpeer-app-c-register-join"></a>

#### register_join

Fonte: `superpeer_app.c:428` (linha nesta revisão; pode mudar em futuras edições).

```c
static int register_join(PeerContext *peer, const Message *message);
```

valida destino, decodifica descritor, recalcula NodeID do remetente e compara ao header; rejeita autorregistro e inclui/atualiza membro antes do ACK.

**Parâmetros**

- `PeerContext *peer`: Contexto local do Super Peer utilizado pelo handler.
- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Funções do projeto usadas diretamente:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`node_init` (node.c)](#fn-node-c-node-init); [`node_id_equal` (node.c)](#fn-node-c-node-id-equal); [`rpc_decode_join_payload` (rpc.c)](#fn-rpc-c-rpc-decode-join-payload); [`print_node_id` (superpeer_app.c)](#fn-superpeer-app-c-print-node-id); [`node_id_is_zero` (superpeer_app.c)](#fn-superpeer-app-c-node-id-is-zero).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EHOSTUNREACH`, `EEXIST`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de register_join, para acompanhar a explicação</summary>

```c
static int register_join(PeerContext *peer, const Message *message)
{
    NodeConfig remote_config;
    Node remote_node;
    SuperPeerRegistrationResult registration;

    if (peer == NULL || message == NULL || peer->superpeer == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (!node_id_is_zero(message->header.destination_node) && memcmp(message->header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) != 0)
    {
        errno = EHOSTUNREACH;
        return -1;
    }

    if (rpc_decode_join_payload(message->payload, message->header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0)
    {
        return -1;
    }

    if (memcmp(remote_node.id.bytes, message->header.source_node, NODE_ID_SIZE) != 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (node_id_equal(&remote_node.id, &peer->local_node.id))
    {
        errno = EEXIST;
        return -1;
    }

    registration = superpeer_register_node(peer->superpeer, &remote_node);
    if (registration == SUPERPEER_REGISTER_ERROR)
    {
        return -1;
    }

    printf("JOIN validado: NodeID=");
    print_node_id(remote_node.id.bytes);
    printf(", resultado=%s, membros=%zu\n", registration == SUPERPEER_MEMBER_ADDED ? "ADDED" : "UPDATED", superpeer_member_count(peer->superpeer));
    return 0;
}
```

</details>

<a id="fn-superpeer-app-c-register-announcement"></a>

#### register_announcement

Fonte: `superpeer_app.c:475` (linha nesta revisão; pode mudar em futuras edições).

```c
static int register_announcement(PeerContext *peer, const Message *message);
```

exige que source_node já esteja cadastrado, decodifica STORE/ANNOUNCE e delega registro de documento/chunks a directory_announce.

**Parâmetros**

- `PeerContext *peer`: Contexto local do Super Peer utilizado pelo handler.
- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Funções do projeto usadas diretamente:** [`directory_announce` (directory.c)](#fn-directory-c-directory-announce); [`superpeer_is_registered` (membership.c)](#fn-membership-c-superpeer-is-registered); [`transfer_decode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-announcement).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EACCES`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de register_announcement, para acompanhar a explicação</summary>

```c
static int register_announcement(PeerContext *peer, const Message *message)
{
    TransferDocument document;
    NodeID owner;

    memcpy(owner.bytes, message->header.source_node, NODE_ID_SIZE);
    if (!superpeer_is_registered(peer->superpeer, &owner))
    {
        errno = EACCES;
        return -1;
    }
    MetadataChunk *chunks = NULL;
    if (transfer_decode_announcement(message->payload, message->header.payload_size, &document, &chunks) < 0)
    {
        return -1;
    }
    int status = directory_announce(peer->directory, &document, chunks, &owner);
    free(chunks);
    return status;
}
```

</details>

<a id="fn-superpeer-app-c-answer-lookup"></a>

#### answer_lookup

Fonte: `superpeer_app.c:496` (linha nesta revisão; pode mudar em futuras edições).

```c
static int answer_lookup(PeerContext *peer, int client_fd, const Message *message);
```

decodifica seletor, consulta Directory, serializa metadados/localizações e responde DOWNLOAD_REP; em erro envia ERROR. Libera estruturas temporárias.

**Parâmetros**

- `PeerContext *peer`: Contexto local do Super Peer utilizado pelo handler.
- `int client_fd`: Descritor conectado, diferente do listener.
- `const Message *message`: Mensagem completa (header e ponteiro de payload); validade e propriedade são descritas no contrato do protocolo.

**Funções do projeto usadas diretamente:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply); [`transfer_decode_lookup_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-request); [`transfer_lookup_result_free` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-lookup-result-free); [`transfer_encode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-result).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_client` (superpeer_app.c)](#fn-superpeer-app-c-handle-client).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `status`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de answer_lookup, para acompanhar a explicação</summary>

```c
static int answer_lookup(PeerContext *peer, int client_fd, const Message *message)
{
    TransferSelectorType type;
    TransferLookupResult result;
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    int status = -1;

    memset(&result, 0, sizeof(result));
    if (transfer_decode_lookup_request(message->payload, message->header.payload_size, &type, &id, name) < 0 || directory_lookup(peer->directory, type, &id, name, &result) < 0 || transfer_encode_lookup_result(&result, &payload, &payload_size) < 0)
    {
        status = send_reply(peer, client_fd, message, M_ERROR, NULL, 0U, 0);
        goto cleanup;
    }
    status = send_reply(peer, client_fd, message, M_DOWNLOAD_REP, payload, payload_size, 0);

cleanup:
    free(payload);
    transfer_lookup_result_free(&result);
    return status;
}
```

</details>

<a id="fn-superpeer-app-c-handle-client"></a>

#### handle_client

Fonte: `superpeer_app.c:521` (linha nesta revisão; pode mudar em futuras edições).

```c
static void handle_client(void *context, int client_fd);
```

callback para conexão TCP. Usa getpeername para obter ip_origem real da conexão, recebe mensagens em laço, registra log e despacha JOIN, PING, LEAVE, ANNOUNCE e LOOKUP; mensagens futuras/inesperadas recebem ERROR. **origem** no log é o NodeID do header (exceto PING, em que é omitido), não o IP. LEAVE remove membro e disponibilidades antes do ACK.

**Parâmetros**

- `void *context`: Contexto compartilhado entregue ao callback; seu tipo real é definido pelo módulo.
- `int client_fd`: Descritor conectado, diferente do listener.

**Funções do projeto usadas diretamente:** [`superpeer_unregister_node` (membership.c)](#fn-membership-c-superpeer-unregister-node); [`metadata_remove_peer` (metadata.c)](#fn-metadata-c-metadata-remove-peer); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`print_node_id` (superpeer_app.c)](#fn-superpeer-app-c-print-node-id); [`message_payload_equals` (superpeer_app.c)](#fn-superpeer-app-c-message-payload-equals); [`node_id_is_zero` (superpeer_app.c)](#fn-superpeer-app-c-node-id-is-zero); [`send_reply` (superpeer_app.c)](#fn-superpeer-app-c-send-reply); [`register_join` (superpeer_app.c)](#fn-superpeer-app-c-register-join); [`register_announcement` (superpeer_app.c)](#fn-superpeer-app-c-register-announcement); [`answer_lookup` (superpeer_app.c)](#fn-superpeer-app-c-answer-lookup).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `getpeername`: Obtém o endereço observado da outra ponta da conexão; não lê o NodeID do header.
- `inet_ntop`: Converte endereço binário para texto legível; não descobre identidade criptográfica.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

**Erros explícitos neste corpo:** `EACCES`, `ENOTSUP`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de handle_client, para acompanhar a explicação</summary>

```c
static void handle_client(void *context, int client_fd)
{
    PeerContext *peer = context;
    Message message;
    struct sockaddr_in remote_address;
    socklen_t remote_address_len = sizeof(remote_address);
    char source_ip[INET_ADDRSTRLEN] = "desconhecido";

    if (getpeername(client_fd, (struct sockaddr *)&remote_address, &remote_address_len) == 0 && remote_address.sin_family == AF_INET)
    {
        (void)inet_ntop(AF_INET, &remote_address.sin_addr, source_ip, sizeof(source_ip));
    }

    message_init(&message);

    for (;;)
    {
        int result = protocol_receive_message(client_fd, &message);

        if (result == PROTOCOL_CLOSED)
        {
            break;
        }
        if (result != PROTOCOL_OK)
        {
            fprintf(stderr, "Falha ao receber uma mensagem do cliente.\n");
            break;
        }

        printf("Mensagem recebida: tipo=%u", (unsigned)message.header.message_type);
        if (message.header.message_type != (uint8_t)M_PING)
        {
            printf(", origem=");
            print_node_id(message.header.source_node);
        }
        printf(", payload=%" PRIu32 " bytes, ip_origem=%s\n", message.header.payload_size, source_ip);
        fflush(stdout);
        if ((message.header.message_type == M_LOOKUP || message.header.message_type == M_STORE) && (node_id_is_zero(message.header.source_node) || memcmp(message.header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) != 0))
        {
            errno = EACCES;
            (void)send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0);
            message_free(&message);
            continue;
        }

        if (message.header.message_type == (uint8_t)M_JOIN)
        {
            Message_Type response_type = register_join(peer, &message) == 0 ? M_ACK : M_ERROR;

            if (response_type == M_ERROR)
            {
                fprintf(stderr, "JOIN rejeitado.\n");
            }
            if (send_reply(peer, client_fd, &message, response_type, NULL, 0U, response_type == M_ACK) < 0)
            {
                fprintf(stderr, "Falha ao enviar resposta ao JOIN.\n");
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == (uint8_t)M_PING)
        {
            if (!message_payload_equals(&message, PING_PAYLOAD))
            {
                fprintf(stderr, "PING rejeitado: payload invalido.\n");
                if (send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0) < 0)
                {
                    message_free(&message);
                    break;
                }
            }
            else
            {
                printf("RX PING\n");
                fflush(stdout);
                if (send_reply(peer, client_fd, &message, M_PONG, (const uint8_t *)PONG_PAYLOAD, (uint32_t)(sizeof(PONG_PAYLOAD) - 1U), 0) < 0)
                {
                    fprintf(stderr, "Falha ao enviar PONG.\n");
                    message_free(&message);
                    break;
                }
            }
        }
        else if (message.header.message_type == (uint8_t)M_LEAVE)
        {
            NodeID departed;
            memcpy(departed.bytes, message.header.source_node, NODE_ID_SIZE);
            if (!node_id_is_zero(departed.bytes) && superpeer_unregister_node(peer->superpeer, &departed) == 0) (void)metadata_remove_peer(peer->metadata, &departed);
            if (send_reply(peer, client_fd, &message, M_ACK, NULL, 0U, 0) < 0)
            {
                fprintf(stderr, "Falha ao enviar ACK de LEAVE.\n");
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == (uint8_t)M_STORE && message.header.payload_size > 0U && message.payload[0] == TRANSFER_STORE_ANNOUNCE)
        {
            Message_Type response_type = register_announcement(peer, &message) == 0 ? M_ACK : M_ERROR;

            if (send_reply(peer, client_fd, &message, response_type, NULL, 0U, 0) < 0)
            {
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == (uint8_t)M_LOOKUP)
        {
            if (answer_lookup(peer, client_fd, &message) < 0)
            {
                message_free(&message);
                break;
            }
        }
        else
        {
            /* Mensagens ainda nao tratadas pelo checkpoint recebem ERROR. */
            errno = ENOTSUP;
            if (send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0) < 0)
            {
                fprintf(stderr, "Falha ao enviar ERROR.\n");
                message_free(&message);
                break;
            }
        }

        message_free(&message);
        message_init(&message);
    }

    message_free(&message);
}
```

</details>

<a id="fn-superpeer-app-c-accept-clients"></a>

#### accept_clients

Fonte: `superpeer_app.c:654` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *accept_clients(void *argument);
```

Executa o laço do runtime que distribui conexões ao pool limitado; não cria threads ilimitadas por cliente.

**Parâmetros**

- `void *argument`: Contexto genérico de pthread/callback, convertido para a estrutura concreta na função.

**Funções do projeto usadas diretamente:** [`concurrent_server_run` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-run).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de accept_clients, para acompanhar a explicação</summary>

```c
static void *accept_clients(void *argument)
{
    PeerContext *peer = (PeerContext *)argument;

    (void)concurrent_server_run(peer->runtime);
    return NULL;
}
```

</details>

<a id="fn-superpeer-app-c-connect-and-join"></a>

#### connect_and_join

Fonte: `superpeer_app.c:663` (linha nesta revisão; pode mudar em futuras edições).

```c
static int connect_and_join(const PeerContext *peer, const char *ip, uint16_t remote_port);
```

forma legada de conectar este servidor a outro nó, enviar JOIN e verificar ACK, TransactionID e identidade recebida; também registra o remoto em sua tabela local.

**Parâmetros**

- `const PeerContext *peer`: Contexto local do Super Peer utilizado pelo handler.
- `const char *ip`: Endereço textual do nó; rede TCP atual usa IPv4 numérico.
- `uint16_t remote_port`: Porta do endpoint remoto, não a escolha do executor local.

**Funções do projeto usadas diretamente:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`network_connect` (network.c)](#fn-network-c-network-connect); [`network_shutdown` (network.c)](#fn-network-c-network-shutdown); [`node_init` (node.c)](#fn-node-c-node-init); [`node_id_equal` (node.c)](#fn-node-c-node-id-equal); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`rpc_decode_join_payload` (rpc.c)](#fn-rpc-c-rpc-decode-join-payload); [`encode_join_payload_alloc` (superpeer_app.c)](#fn-superpeer-app-c-encode-join-payload-alloc); [`transfer_fill_transaction_id` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-fill-transaction-id).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `result == PROTOCOL_OK ? 0 : -1`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de connect_and_join, para acompanhar a explicação</summary>

```c
static int connect_and_join(const PeerContext *peer, const char *ip, uint16_t remote_port)
{
    int socket_fd = network_connect(ip, remote_port);
    Message join;
    Message response;
    NodeConfig remote_config;
    Node remote_node;
    SuperPeerRegistrationResult registration;
    int result;

    if (socket_fd < 0)
    {
        return -1;
    }

    message_init(&join);
    join.header.message_type = (uint8_t)M_JOIN;
    memcpy(join.header.source_node, peer->local_node.id.bytes, NODE_ID_SIZE);
    memset(join.header.destination_node, 0, NODE_ID_SIZE);
    transfer_fill_transaction_id(join.header.transaction_id, join.header.source_node);
    join.header.timestamp = (uint64_t)time(NULL);
    join.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;

    if (encode_join_payload_alloc(&peer->local_node.config, &join.payload) < 0)
    {
        fprintf(stderr, "Nao foi possivel serializar o payload de JOIN.\n");
        message_free(&join);
        (void)network_shutdown(socket_fd);
        return -1;
    }

    if (protocol_send_message(socket_fd, &join) < 0)
    {
        fprintf(stderr, "Falha ao enviar JOIN.\n");
        message_free(&join);
        (void)network_shutdown(socket_fd);
        return -1;
    }

    message_init(&response);
    result = protocol_receive_message(socket_fd, &response);
    if (result == PROTOCOL_OK)
    {
        if (response.header.message_type != (uint8_t)M_ACK)
        {
            fprintf(stderr, "O peer remoto rejeitou o JOIN (tipo=%u).\n", (unsigned)response.header.message_type);
            result = PROTOCOL_ERROR;
        }
        else if (memcmp(response.header.transaction_id, join.header.transaction_id, TRANSACTION_ID_SIZE) != 0 || memcmp(response.header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) != 0)
        {
            fprintf(stderr, "A resposta possui identificadores diferentes.\n");
            result = PROTOCOL_ERROR;
        }
        else if (rpc_decode_join_payload(response.payload, response.header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0 || memcmp(remote_node.id.bytes, response.header.source_node, NODE_ID_SIZE) != 0 || node_id_equal(&remote_node.id, &peer->local_node.id))
        {
            fprintf(stderr, "A identidade do peer remoto e invalida.\n");
            result = PROTOCOL_ERROR;
        }
        else if ((registration = superpeer_register_node(peer->superpeer, &remote_node)) == SUPERPEER_REGISTER_ERROR)
        {
            fprintf(stderr, "Nao foi possivel registrar o peer remoto.\n");
            result = PROTOCOL_ERROR;
        }
        else
        {
            printf("JOIN aceito pelo peer remoto: tipo=%u, membro=%s, " "membros=%zu\n", (unsigned)response.header.message_type, registration == SUPERPEER_MEMBER_ADDED ? "ADDED" : "UPDATED", superpeer_member_count(peer->superpeer));
        }
    }
    else if (result == PROTOCOL_CLOSED)
    {
        fprintf(stderr, "O peer remoto fechou a conexao sem responder.\n");
    }
    else
    {
        fprintf(stderr, "Falha ao receber a resposta do JOIN.\n");
    }

    message_free(&join);
    message_free(&response);
    (void)network_shutdown(socket_fd);
    return result == PROTOCOL_OK ? 0 : -1;
}
```

</details>

<a id="fn-superpeer-app-c-execute-command"></a>

#### execute_command

Fonte: `superpeer_app.c:747` (linha nesta revisão; pode mudar em futuras edições).

```c
static int execute_command(const NodeArguments *arguments);
```

modo --cmd de bin/superpeer/bin/node para PING, JOIN ou LEAVE. Monta mensagem, envia uma vez, confere resposta e encerra.

**Parâmetros**

- `const NodeArguments *arguments`: Estrutura com argumentos já interpretados.

**Funções do projeto usadas diretamente:** [`network_connect` (network.c)](#fn-network-c-network-connect); [`network_shutdown` (network.c)](#fn-network-c-network-shutdown); [`node_config_init` (node.c)](#fn-node-c-node-config-init); [`node_init` (node.c)](#fn-node-c-node-init); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`message_type_name` (superpeer_app.c)](#fn-superpeer-app-c-message-type-name); [`set_text_payload` (superpeer_app.c)](#fn-superpeer-app-c-set-text-payload); [`message_payload_equals` (superpeer_app.c)](#fn-superpeer-app-c-message-payload-equals); [`encode_join_payload_alloc` (superpeer_app.c)](#fn-superpeer-app-c-encode-join-payload-alloc); [`transfer_fill_transaction_id` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-fill-transaction-id).

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.

**Retorno:** as expressões presentes nesta função são `-1`, `result`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de execute_command, para acompanhar a explicação</summary>

```c
static int execute_command(const NodeArguments *arguments)
{
    int socket_fd;
    int result = -1;
    Message request;
    Message response;
    Message_Type expected_type;
    NodeConfig config;
    Node node;

    if (arguments == NULL || !arguments->command_mode || arguments->remote_ip == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    socket_fd = network_connect(arguments->remote_ip, arguments->remote_port);
    if (socket_fd < 0)
    {
        return -1;
    }

    if (message_init(&request) != PROTOCOL_OK || message_init(&response) != PROTOCOL_OK)
    {
        (void)network_shutdown(socket_fd);
        return -1;
    }

    request.header.message_type = (uint8_t)arguments->command_type;
    if (arguments->command_type == M_PING)
    {
        expected_type = M_PONG;
        if (set_text_payload(&request, PING_PAYLOAD) < 0)
        {
            goto cleanup;
        }
    }
    else if (arguments->command_type == M_JOIN)
    {
        expected_type = M_ACK;
        if (node_config_init(&config, DEFAULT_LOCAL_IP, 1U) < 0 || node_init(&node, &config) < 0 || encode_join_payload_alloc(&config, &request.payload) < 0)
        {
            goto cleanup;
        }
        request.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;
        memcpy(request.header.source_node, node.id.bytes, NODE_ID_SIZE);
    }
    else if (arguments->command_type == M_LEAVE)
    {
        expected_type = M_ACK;
    }
    else
    {
        errno = EINVAL;
        goto cleanup;
    }

    transfer_fill_transaction_id(request.header.transaction_id, request.header.source_node);
    request.header.timestamp = (uint64_t)time(NULL);
    printf("TX %s\n", message_type_name(arguments->command_type));
    fflush(stdout);

    if (protocol_send_message(socket_fd, &request) != PROTOCOL_OK)
    {
        fprintf(stderr, "Falha ao enviar %s.\n", message_type_name(arguments->command_type));
        goto cleanup;
    }

    if (protocol_receive_message(socket_fd, &response) != PROTOCOL_OK)
    {
        fprintf(stderr, "Falha ao receber a resposta.\n");
        goto cleanup;
    }
    if (response.header.message_type != (uint8_t)expected_type || memcmp(response.header.transaction_id, request.header.transaction_id, TRANSACTION_ID_SIZE) != 0)
    {
        fprintf(stderr, "Resposta invalida: esperado %s, recebido %s.\n", message_type_name(expected_type), message_type_name((Message_Type)response.header.message_type));
        goto cleanup;
    }
    if (expected_type == M_PONG && !message_payload_equals(&response, PONG_PAYLOAD))
    {
        fprintf(stderr, "PONG recebido com payload invalido.\n");
        goto cleanup;
    }

    printf("RX %s\n", message_type_name(expected_type));
    fflush(stdout);
    result = 0;

cleanup:
    message_free(&request);
    message_free(&response);
    (void)network_shutdown(socket_fd);
    return result;
}
```

</details>

<a id="fn-superpeer-app-c-print-usage"></a>

#### print_usage

Fonte: `superpeer_app.c:845` (linha nesta revisão; pode mudar em futuras edições).

```c
static void print_usage(const char *program_name);
```

mostra os modos aceitos pelo Super Peer, incluindo interface posicional legada.

**Parâmetros**

- `const char *program_name`: Nome do executável usado na mensagem de ajuda.

**Chamadores diretos encontrados nos fontes inventariados:** [`superpeer_run` (superpeer_app.c)](#fn-superpeer-app-c-superpeer-run).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de print_usage, para acompanhar a explicação</summary>

```c
static void print_usage(const char *program_name)
{
    fprintf(stderr, "Uso: %s <porta-local> [<ip-remoto> <porta-remota>]\n" "   ou: %s --config <arquivo> --port <porta> --name <nome>\n" "   ou: %s --cmd <ping|join|leave> --host <ip> --port <porta>\n", program_name, program_name, program_name);
}
```

</details>

<a id="fn-superpeer-app-c-superpeer-run"></a>

#### superpeer_run

Fonte: `superpeer_app.c:851` (linha nesta revisão; pode mudar em futuras edições).

```c
int superpeer_run(int argc, char **argv);
```

inicializa modo servidor ou executa comando único; no modo servidor cria identidade, MetadataStore, Directory e socket, inicia atendimento e aguarda sinal. No encerramento para conexões e libera recursos na ordem inversa. É chamado pelo main exclusivo de superpeer.c.

**Parâmetros**

- `int argc`: Quantidade de argumentos; quando ponteiro, a função pode atualizar a contagem após retirar opções.
- `char **argv`: Vetor de argumentos terminados em NUL; as rotinas de configuração/CLI podem reorganizar seus ponteiros.

**Funções do projeto usadas diretamente:** [`app_config_load` (app_config.c)](#fn-app-config-c-app-config-load); [`concurrent_server_create` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-create); [`concurrent_server_stop` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-stop); [`concurrent_server_destroy` (concurrent_server.c)](#fn-concurrent-server-c-concurrent-server-destroy); [`directory_create` (directory.c)](#fn-directory-c-directory-create); [`directory_destroy` (directory.c)](#fn-directory-c-directory-destroy); [`superpeer_destroy` (membership.c)](#fn-membership-c-superpeer-destroy); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`metadata_create` (metadata.c)](#fn-metadata-c-metadata-create); [`metadata_destroy` (metadata.c)](#fn-metadata-c-metadata-destroy); [`network_create_server` (network.c)](#fn-network-c-network-create-server); [`network_shutdown` (network.c)](#fn-network-c-network-shutdown); [`parse_node_arguments` (superpeer_app.c)](#fn-superpeer-app-c-parse-node-arguments); [`print_node_id` (superpeer_app.c)](#fn-superpeer-app-c-print-node-id); [`initialize_local_identity` (superpeer_app.c)](#fn-superpeer-app-c-initialize-local-identity); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command); [`print_usage` (superpeer_app.c)](#fn-superpeer-app-c-print-usage).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (superpeer.c)](#fn-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `pthread_create`: Inicia thread com callback e contexto; erro vem no retorno pthread, não necessariamente em errno.
- `pthread_join`: Aguarda a thread, garantindo que o contexto dela não seja liberado enquanto ainda estiver em uso.

**Retorno:** as expressões presentes nesta função são `EXIT_FAILURE`, `execute_command(&arguments) == 0 ? EXIT_SUCCESS : EXIT_FAILURE`, `EXIT_SUCCESS`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de superpeer_run, para acompanhar a explicação</summary>

```c
int superpeer_run(int argc, char **argv)
{
    NodeArguments arguments;
    PeerContext peer;
    pthread_t accept_thread;
    int thread_result;

    if (app_config_load(&argc, argv, 1) < 0 || parse_node_arguments(argc, argv, &arguments) < 0)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    (void)signal(SIGPIPE, SIG_IGN);

    if (arguments.command_mode)
    {
        return execute_command(&arguments) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    memset(&peer, 0, sizeof(peer));
    peer.server_fd = -1;
    if (initialize_local_identity(&peer, arguments.local_port) < 0)
    {
        fprintf(stderr, "Nao foi possivel inicializar a identidade local.\n");
        return EXIT_FAILURE;
    }
    if (metadata_create(&peer.metadata) < 0 || directory_create(peer.metadata, peer.superpeer, &peer.directory) < 0)
    {
        fprintf(stderr, "Nao foi possivel inicializar o diretorio de metadados.\n");
        metadata_destroy(peer.metadata);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    peer.server_fd = network_create_server(arguments.local_port, PEER_BACKLOG);
    if (peer.server_fd < 0 || concurrent_server_create(peer.server_fd, handle_client, &peer, &peer.runtime) < 0)
    {
        if (peer.server_fd >= 0)
        {
            (void)network_shutdown(peer.server_fd);
        }
        directory_destroy(peer.directory);
        metadata_destroy(peer.metadata);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    printf("Node %s started\n", arguments.node_name);
    printf("NodeID: ");
    print_node_id(peer.local_node.id.bytes);
    printf("\n");
    printf("Peer ouvindo na porta %" PRIu16 ", NodeID=", arguments.local_port);
    print_node_id(peer.local_node.id.bytes);
    printf(", membros locais=%zu\n", superpeer_member_count(peer.superpeer));
    fflush(stdout);

    thread_result = pthread_create(&accept_thread, NULL, accept_clients, &peer);
    if (thread_result != 0)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(thread_result));
        concurrent_server_destroy(peer.runtime);
        directory_destroy(peer.directory);
        metadata_destroy(peer.metadata);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    if (arguments.remote_ip != NULL && connect_and_join(&peer, arguments.remote_ip, arguments.remote_port) < 0)
    {
        fprintf(stderr, "Nao foi possivel concluir o JOIN remoto.\n");
    }

    /* Mantem o processo vivo para aceitar novos clientes. */
    while (g_running)
    {
        (void)sleep(1U);
    }

    /* Interrompe novas conexões antes de encerrar clientes e liberar o estado compartilhado. */
    concurrent_server_stop(peer.runtime);
    (void)pthread_join(accept_thread, NULL);
    concurrent_server_destroy(peer.runtime);
    directory_destroy(peer.directory);
    metadata_destroy(peer.metadata);
    superpeer_destroy(peer.superpeer);
    printf("Peer encerrado.\n");
    return EXIT_SUCCESS;
}
```

</details>

<a id="mod-test-node-superpeer-c"></a>

### test_node_superpeer.c

[test_node_id_is_deterministic](#fn-test-node-superpeer-c-test-node-id-is-deterministic) · [test_node_records_process_and_round_trips_id](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id) · [test_node_rejects_invalid_configuration](#fn-test-node-superpeer-c-test-node-rejects-invalid-configuration) · [test_superpeer_registers_and_finds_members](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members) · [test_superpeer_rejects_inconsistent_nodes_and_unregisters](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters) · [register_members](#fn-test-node-superpeer-c-register-members) · [test_superpeer_members_are_thread_safe](#fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe) · [main](#fn-test-node-superpeer-c-main)

<a id="fn-test-node-superpeer-c-test-node-id-is-deterministic"></a>

#### test_node_id_is_deterministic

Fonte: `test_node_superpeer.c:15` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_node_id_is_deterministic(void);
```

usa IP, porta e UUID fixos e compara NodeID com hash conhecido.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid); [`node_compute_id` (node.c)](#fn-node-c-node-compute-id); [`node_id_to_hex` (node.c)](#fn-node-c-node-id-to-hex).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (test_node_superpeer.c)](#fn-test-node-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_node_id_is_deterministic, para acompanhar a explicação</summary>

```c
static void test_node_id_is_deterministic(void)
{
    static const uint8_t uuid[NODE_UUID_SIZE] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    NodeConfig config;
    NodeID node_id;
    char node_id_hex[NODE_ID_HEX_SIZE];

    assert(node_config_init_with_uuid(&config, "127.0.0.1", 8080, uuid) == 0);
    assert(node_compute_id(&config, &node_id) == 0);
    assert(node_id_to_hex(&node_id, node_id_hex, sizeof(node_id_hex)) == 0);
    assert(strcmp(node_id_hex, "9bc987770f04725d8afac342e3eaa1d2" "ef4bc1f4587841f8dd4b8b0fca39f84f") == 0);
}
```

</details>

<a id="fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id"></a>

#### test_node_records_process_and_round_trips_id

Fonte: `test_node_superpeer.c:32` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_node_records_process_and_round_trips_id(void);
```

verifica PID/papel, IPv6, conversão hexadecimal reversível, comparação de IDs e erro de buffer pequeno.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid); [`node_init` (node.c)](#fn-node-c-node-init); [`node_validate` (node.c)](#fn-node-c-node-validate); [`node_id_to_hex` (node.c)](#fn-node-c-node-id-to-hex); [`node_id_from_hex` (node.c)](#fn-node-c-node-id-from-hex); [`node_id_compare` (node.c)](#fn-node-c-node-id-compare); [`node_id_equal` (node.c)](#fn-node-c-node-id-equal); [`node_get_process_id` (node.c)](#fn-node-c-node-get-process-id).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (test_node_superpeer.c)](#fn-test-node-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_node_records_process_and_round_trips_id, para acompanhar a explicação</summary>

```c
static void test_node_records_process_and_round_trips_id(void)
{
    static const uint8_t uuid[NODE_UUID_SIZE] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x00
    };
    NodeConfig config;
    Node node;
    NodeID parsed_id;
    NodeID lower_id = {{0}};
    NodeID higher_id = {{0}};
    char node_id_hex[NODE_ID_HEX_SIZE];
    char too_small[NODE_ID_HEX_SIZE - 1U];

    assert(node_config_init_with_uuid(&config, "::1", 9000, uuid) == 0);
    assert(node_init(&node, &config) == 0);
    assert(node_get_process_id(&node) == getpid());
    assert(node.role == NODE_ROLE_PEER);
    assert(node_validate(&node) == 0);
    assert(node_id_to_hex(&node.id, node_id_hex, sizeof(node_id_hex)) == 0);
    assert(node_id_from_hex(&parsed_id, node_id_hex) == 0);
    assert(node_id_equal(&node.id, &parsed_id));
    assert(node_id_compare(&node.id, &parsed_id) == 0);
    higher_id.bytes[NODE_ID_SIZE - 1U] = 1U;
    assert(node_id_compare(&lower_id, &higher_id) < 0);
    assert(node_id_compare(&higher_id, &lower_id) > 0);

    errno = 0;
    assert(node_id_to_hex(&node.id, too_small, sizeof(too_small)) == -1);
    assert(errno == ENOSPC);
}
```

</details>

<a id="fn-test-node-superpeer-c-test-node-rejects-invalid-configuration"></a>

#### test_node_rejects_invalid_configuration

Fonte: `test_node_superpeer.c:65` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_node_rejects_invalid_configuration(void);
```

exercita IP inválido, porta zero, IP sem NUL e UUID gerado.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`node_config_validate` (node.c)](#fn-node-c-node-config-validate); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid); [`node_config_init` (node.c)](#fn-node-c-node-config-init).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (test_node_superpeer.c)](#fn-test-node-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_node_rejects_invalid_configuration, para acompanhar a explicação</summary>

```c
static void test_node_rejects_invalid_configuration(void)
{
    static const uint8_t uuid[NODE_UUID_SIZE] = {0};
    NodeConfig config;
    NodeConfig malformed_config;

    assert(node_config_init_with_uuid(&config, "not-an-ip", 9000, uuid) == -1);
    assert(node_config_init_with_uuid(&config, "127.0.0.1", 0, uuid) == -1);
    assert(node_config_init(&config, "127.0.0.1", 9001) == 0);
    assert(node_config_validate(&config) == 0);

    memset(&malformed_config, 'x', sizeof(malformed_config));
    malformed_config.port = 9002U;
    assert(node_config_validate(&malformed_config) == -1);
}
```

</details>

<a id="fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members"></a>

#### test_superpeer_registers_and_finds_members

Fonte: `test_node_superpeer.c:82` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_superpeer_registers_and_finds_members(void);
```

confere autorregistro, crescimento da tabela, inclusão, consulta e atualização sem duplicata.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`superpeer_config_init` (membership.c)](#fn-membership-c-superpeer-config-init); [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`superpeer_destroy` (membership.c)](#fn-membership-c-superpeer-destroy); [`superpeer_get_node` (membership.c)](#fn-membership-c-superpeer-get-node); [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_find_member` (membership.c)](#fn-membership-c-superpeer-find-member); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid); [`node_init` (node.c)](#fn-node-c-node-init); [`node_validate` (node.c)](#fn-node-c-node-validate); [`node_id_equal` (node.c)](#fn-node-c-node-id-equal).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (test_node_superpeer.c)](#fn-test-node-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_superpeer_registers_and_finds_members, para acompanhar a explicação</summary>

```c
static void test_superpeer_registers_and_finds_members(void)
{
    static const uint8_t member_uuid[NODE_UUID_SIZE] = {
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };
    SuperPeerConfig config;
    SuperPeer *superpeer = NULL;
    Node member;
    Node local_node;
    SuperPeerMember found_member;
    NodeConfig member_config;

    assert(superpeer_config_init(&config, "127.0.0.1", 7000) == 0);
    config.initial_member_capacity = 1U;
    assert(superpeer_create(&config, &superpeer) == 0);
    assert(superpeer_get_node(superpeer, &local_node) == 0);
    assert(local_node.role == NODE_ROLE_SUPERPEER);
    assert(node_validate(&local_node) == 0);
    assert(superpeer_member_count(superpeer) == 1U);
    assert(superpeer_find_member(superpeer, &local_node.id, &found_member) == 0);
    assert(found_member.state == SUPERPEER_MEMBER_ALIVE);

    assert(node_config_init_with_uuid(&member_config, "127.0.0.2", 7001, member_uuid) == 0);
    assert(node_init(&member, &member_config) == 0);
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_MEMBER_ADDED);
    assert(superpeer_member_count(superpeer) == 2U);
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_MEMBER_UPDATED);
    assert(superpeer_member_count(superpeer) == 2U);
    assert(superpeer_find_member(superpeer, &member.id, &found_member) == 0);
    assert(node_id_equal(&found_member.node.id, &member.id));
    assert(found_member.node.config.port == 7001U);

    superpeer_destroy(superpeer);
}
```

</details>

<a id="fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters"></a>

#### test_superpeer_rejects_inconsistent_nodes_and_unregisters

Fonte: `test_node_superpeer.c:119` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_superpeer_rejects_inconsistent_nodes_and_unregisters(void);
```

testa ID adulterado, proteção do nó local, remoção de membro e ausência.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`superpeer_config_init_with_uuid` (membership.c)](#fn-membership-c-superpeer-config-init-with-uuid); [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`superpeer_destroy` (membership.c)](#fn-membership-c-superpeer-destroy); [`superpeer_get_node` (membership.c)](#fn-membership-c-superpeer-get-node); [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`superpeer_unregister_node` (membership.c)](#fn-membership-c-superpeer-unregister-node); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count); [`superpeer_is_registered` (membership.c)](#fn-membership-c-superpeer-is-registered); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid); [`node_init` (node.c)](#fn-node-c-node-init).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (test_node_superpeer.c)](#fn-test-node-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_superpeer_rejects_inconsistent_nodes_and_unregisters, para acompanhar a explicação</summary>

```c
static void test_superpeer_rejects_inconsistent_nodes_and_unregisters(void)
{
    static const uint8_t local_uuid[NODE_UUID_SIZE] = {1};
    static const uint8_t member_uuid[NODE_UUID_SIZE] = {2};
    SuperPeerConfig config;
    SuperPeer *superpeer = NULL;
    Node member;
    Node local_node;
    NodeConfig member_config;
    NodeID unknown_id = {{0}};

    assert(superpeer_config_init_with_uuid(&config, "127.0.0.1", 7100, local_uuid) == 0);
    assert(superpeer_create(&config, &superpeer) == 0);
    assert(superpeer_get_node(superpeer, &local_node) == 0);
    errno = 0;
    assert(superpeer_unregister_node(superpeer, &local_node.id) == -1);
    assert(errno == EPERM);
    assert(superpeer_member_count(superpeer) == 1U);
    assert(node_config_init_with_uuid(&member_config, "127.0.0.2", 7101, member_uuid) == 0);
    assert(node_init(&member, &member_config) == 0);

    member.id.bytes[0] ^= 0xffU;
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_REGISTER_ERROR);
    assert(superpeer_member_count(superpeer) == 1U);

    assert(node_init(&member, &member_config) == 0);
    assert(superpeer_register_node(superpeer, &member) == SUPERPEER_MEMBER_ADDED);
    assert(superpeer_is_registered(superpeer, &member.id));
    assert(superpeer_unregister_node(superpeer, &member.id) == 0);
    assert(!superpeer_is_registered(superpeer, &member.id));
    assert(superpeer_member_count(superpeer) == 1U);
    assert(superpeer_unregister_node(superpeer, &unknown_id) == -1);

    superpeer_destroy(superpeer);
}
```

</details>

<a id="fn-test-node-superpeer-c-register-members"></a>

#### register_members

Fonte: `test_node_superpeer.c:169` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *register_members(void *argument);
```

corpo executado por cada thread do teste de concorrência; registra 25 nós diferentes.

**Parâmetros**

- `void *argument`: Contexto genérico de pthread/callback, convertido para a estrutura concreta na função.

**Funções do projeto usadas diretamente:** [`superpeer_register_node` (membership.c)](#fn-membership-c-superpeer-register-node); [`node_config_init_with_uuid` (node.c)](#fn-node-c-node-config-init-with-uuid); [`node_init` (node.c)](#fn-node-c-node-init).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de register_members, para acompanhar a explicação</summary>

```c
static void *register_members(void *argument)
{
    RegistrationContext *context = argument;
    unsigned int i;

    for (i = 0U; i < REGISTRATIONS_PER_THREAD; ++i)
    {
        uint8_t uuid[NODE_UUID_SIZE] = {0};
        NodeConfig node_config;
        Node node;
        uint16_t port = (uint16_t)(7200U + context->thread_number * 100U + i);

        uuid[0] = (uint8_t)context->thread_number;
        uuid[1] = (uint8_t)i;
        if (node_config_init_with_uuid(&node_config, "127.0.0.1", port, uuid) == -1 || node_init(&node, &node_config) == -1 || superpeer_register_node(context->superpeer, &node) != SUPERPEER_MEMBER_ADDED)
        {
            context->failed = 1;
            return NULL;
        }
    }
    return NULL;
}
```

</details>

<a id="fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe"></a>

#### test_superpeer_members_are_thread_safe

Fonte: `test_node_superpeer.c:193` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_superpeer_members_are_thread_safe(void);
```

cria quatro threads de registro, espera todas e confere 101 membros incluindo o local. Exercita sincronização, mas não é prova absoluta de ausência de corridas.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`superpeer_config_init_with_uuid` (membership.c)](#fn-membership-c-superpeer-config-init-with-uuid); [`superpeer_create` (membership.c)](#fn-membership-c-superpeer-create); [`superpeer_destroy` (membership.c)](#fn-membership-c-superpeer-destroy); [`superpeer_member_count` (membership.c)](#fn-membership-c-superpeer-member-count).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (test_node_superpeer.c)](#fn-test-node-superpeer-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `pthread_create`: Inicia thread com callback e contexto; erro vem no retorno pthread, não necessariamente em errno.
- `pthread_join`: Aguarda a thread, garantindo que o contexto dela não seja liberado enquanto ainda estiver em uso.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_superpeer_members_are_thread_safe, para acompanhar a explicação</summary>

```c
static void test_superpeer_members_are_thread_safe(void)
{
    static const uint8_t local_uuid[NODE_UUID_SIZE] = {3};
    SuperPeerConfig config;
    SuperPeer *superpeer = NULL;
    RegistrationContext contexts[REGISTRATION_THREAD_COUNT];
    pthread_t threads[REGISTRATION_THREAD_COUNT];
    unsigned int i;

    assert(superpeer_config_init_with_uuid(&config, "127.0.0.1", 7300, local_uuid) == 0);
    config.initial_member_capacity = 1U;
    assert(superpeer_create(&config, &superpeer) == 0);

    for (i = 0U; i < REGISTRATION_THREAD_COUNT; ++i)
    {
        contexts[i].superpeer = superpeer;
        contexts[i].thread_number = i;
        contexts[i].failed = 0;
        assert(pthread_create(&threads[i], NULL, register_members, &contexts[i]) == 0);
    }
    for (i = 0U; i < REGISTRATION_THREAD_COUNT; ++i)
    {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(contexts[i].failed == 0);
    }

    assert(superpeer_member_count(superpeer) == 1U + REGISTRATION_THREAD_COUNT * REGISTRATIONS_PER_THREAD);
    superpeer_destroy(superpeer);
}
```

</details>

<a id="fn-test-node-superpeer-c-main"></a>

#### main

Fonte: `test_node_superpeer.c:224` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(void);
```

Executa os seis cenários da API local de identidade e membership. Não inicia servidores TCP. Assert falso aborta o processo; se todos passam, imprime confirmação e retorna sucesso.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`test_node_id_is_deterministic` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-id-is-deterministic); [`test_node_records_process_and_round_trips_id` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-records-process-and-round-trips-id); [`test_node_rejects_invalid_configuration` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-node-rejects-invalid-configuration); [`test_superpeer_registers_and_finds_members` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-registers-and-finds-members); [`test_superpeer_rejects_inconsistent_nodes_and_unregisters` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-rejects-inconsistent-nodes-and-unregisters); [`test_superpeer_members_are_thread_safe` (test_node_superpeer.c)](#fn-test-node-superpeer-c-test-superpeer-members-are-thread-safe).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Retorno:** as expressões presentes nesta função são `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(void)
{
    test_node_id_is_deterministic();
    test_node_records_process_and_round_trips_id();
    test_node_rejects_invalid_configuration();
    test_superpeer_registers_and_finds_members();
    test_superpeer_rejects_inconsistent_nodes_and_unregisters();
    test_superpeer_members_are_thread_safe();
    puts("node/superpeer tests: ok");
    return 0;
}
```

</details>

<a id="mod-teste-c"></a>

### teste.c

[main](#fn-teste-c-main)

<a id="fn-teste-c-main"></a>

#### main

Fonte: `teste.c:4` (linha nesta revisão; pode mudar em futuras edições).

```c
int main();
```

Programa demonstrativo isolado que imprime a macro __STDC_VERSION__, informando a versão do padrão C selecionada pelo compilador. Não faz parte dos executáveis Peer/Super Peer nem dos testes do Makefile.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Entrada/chamada:** iniciada pelo runtime do executável.

**Retorno:** as expressões presentes nesta função são `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main()
{
    printf("%ld\n", __STDC_VERSION__);
    return 0;
}
```

</details>

<a id="mod-transfer-protocol-c"></a>

### transfer_protocol.c

[bounded_string_length](#fn-transfer-protocol-c-bounded-string-length) · [object_id_from_hex](#fn-transfer-protocol-c-object-id-from-hex) · [document_wire_size](#fn-transfer-protocol-c-document-wire-size) · [encode_document_at](#fn-transfer-protocol-c-encode-document-at) · [decode_document_at](#fn-transfer-protocol-c-decode-document-at) · [transfer_fill_transaction_id](#fn-transfer-protocol-c-transfer-fill-transaction-id) · [transfer_encode_document](#fn-transfer-protocol-c-transfer-encode-document) · [transfer_decode_document](#fn-transfer-protocol-c-transfer-decode-document) · [transfer_encode_chunk](#fn-transfer-protocol-c-transfer-encode-chunk) · [transfer_decode_chunk](#fn-transfer-protocol-c-transfer-decode-chunk) · [transfer_encode_object_operation](#fn-transfer-protocol-c-transfer-encode-object-operation) · [transfer_decode_object_operation](#fn-transfer-protocol-c-transfer-decode-object-operation) · [transfer_encode_lookup_request](#fn-transfer-protocol-c-transfer-encode-lookup-request) · [transfer_decode_lookup_request](#fn-transfer-protocol-c-transfer-decode-lookup-request) · [transfer_lookup_result_free](#fn-transfer-protocol-c-transfer-lookup-result-free) · [transfer_encode_lookup_result](#fn-transfer-protocol-c-transfer-encode-lookup-result) · [transfer_decode_lookup_result](#fn-transfer-protocol-c-transfer-decode-lookup-result) · [transfer_encode_chunk_request](#fn-transfer-protocol-c-transfer-encode-chunk-request) · [transfer_decode_chunk_request](#fn-transfer-protocol-c-transfer-decode-chunk-request) · [encode_descriptor](#fn-transfer-protocol-c-encode-descriptor) · [decode_descriptor](#fn-transfer-protocol-c-decode-descriptor) · [transfer_encode_announcement](#fn-transfer-protocol-c-transfer-encode-announcement) · [transfer_decode_announcement](#fn-transfer-protocol-c-transfer-decode-announcement)

<a id="fn-transfer-protocol-c-bounded-string-length"></a>

#### bounded_string_length

Fonte: `transfer_protocol.c:22` (linha nesta revisão; pode mudar em futuras edições).

```c
static size_t bounded_string_length(const char *text, size_t capacity);
```

procura o terminador NUL sem ler além da capacidade. Serve para validar nomes e IPs de tamanho fixo.

**Parâmetros**

- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.
- `size_t capacity`: Capacidade disponível para escrita ou limite de busca; não indica conteúdo já preenchido.

**Chamadores diretos encontrados nos fontes inventariados:** [`document_wire_size` (transfer_protocol.c)](#fn-transfer-protocol-c-document-wire-size); [`encode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-document-at); [`transfer_encode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-result).

**Retorno:** as expressões presentes nesta função são `length`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de bounded_string_length, para acompanhar a explicação</summary>

```c
static size_t bounded_string_length(const char *text, size_t capacity)
{
    size_t length = 0U;

    while (length < capacity && text[length] != '\0')
    {
        ++length;
    }
    return length;
}
```

</details>

<a id="fn-transfer-protocol-c-object-id-from-hex"></a>

#### object_id_from_hex

Fonte: `transfer_protocol.c:39` (linha nesta revisão; pode mudar em futuras edições).

```c
static int object_id_from_hex(ObjectID *id, const char *text);
```

aceita exatamente 64 dígitos hexadecimais e converte para os 32 bytes do ObjectID. É usado para distinguir um seletor por ID de um nome de arquivo.

**Parâmetros**

- `ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `const char *text`: Texto de entrada; quando char* mutável, pode ser ajustado no próprio buffer.

**Chamadores diretos encontrados nos fontes inventariados:** [`transfer_encode_lookup_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-request).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de object_id_from_hex, para acompanhar a explicação</summary>

```c
static int object_id_from_hex(ObjectID *id, const char *text)
{
    size_t index;

    if (id == NULL || text == NULL || strlen(text) != OBJECT_ID_SIZE * 2U)
    {
        return -1;
    }
    for (index = 0U; index < OBJECT_ID_SIZE; ++index)
    {
        int high;
        int low;
        char a = text[index * 2U];
        char b = text[index * 2U + 1U];

        high = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
        low = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
        if (high < 0 || low < 0)
        {
            return -1;
        }
        id->bytes[index] = (uint8_t)((high << 4) | low);
    }
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-document-wire-size"></a>

#### document_wire_size

Fonte: `transfer_protocol.c:65` (linha nesta revisão; pode mudar em futuras edições).

```c
static size_t document_wire_size(const TransferDocument *document);
```

calcula o espaço necessário para operação, campos fixos e nome sem NUL.

**Parâmetros**

- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Funções do projeto usadas diretamente:** [`bounded_string_length` (transfer_protocol.c)](#fn-transfer-protocol-c-bounded-string-length).

**Chamadores diretos encontrados nos fontes inventariados:** [`transfer_encode_document` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-document); [`transfer_encode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-result); [`transfer_encode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-announcement).

**Retorno:** as expressões presentes nesta função são `1U + DOCUMENT_FIXED_SIZE + bounded_string_length(document->name, METADATA_NAME_SIZE)`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de document_wire_size, para acompanhar a explicação</summary>

```c
static size_t document_wire_size(const TransferDocument *document)
{
    return 1U + DOCUMENT_FIXED_SIZE + bounded_string_length(document->name, METADATA_NAME_SIZE);
}
```

</details>

<a id="fn-transfer-protocol-c-encode-document-at"></a>

#### encode_document_at

Fonte: `transfer_protocol.c:70` (linha nesta revisão; pode mudar em futuras edições).

```c
static int encode_document_at(const TransferDocument *document, uint8_t operation, uint8_t *output, size_t capacity, size_t *used);
```

valida nome, LZ4, contagem esperada de chunks e espaço; escreve operação, ObjectID, tamanho, contagem, compressão e nome. Informa bytes efetivamente usados.

**Parâmetros**

- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `uint8_t operation`: Suboperação do payload; diferente do tipo do header.
- `uint8_t *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.
- `size_t capacity`: Capacidade disponível para escrita ou limite de busca; não indica conteúdo já preenchido.
- `size_t *used`: Ponteiro para a quantidade de bytes consumidos/escritos.

**Funções do projeto usadas diretamente:** [`bounded_string_length` (transfer_protocol.c)](#fn-transfer-protocol-c-bounded-string-length); [`wire_put_u16` (wire.h)](#fn-wire-h-wire-put-u16); [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`transfer_encode_document` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-document); [`transfer_encode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-result); [`transfer_encode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-announcement).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de encode_document_at, para acompanhar a explicação</summary>

```c
static int encode_document_at(const TransferDocument *document, uint8_t operation, uint8_t *output, size_t capacity, size_t *used)
{
    size_t name_size;
    size_t offset = 0U;

    if (document == NULL || output == NULL || used == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    name_size = bounded_string_length(document->name, METADATA_NAME_SIZE);
    if (name_size == 0U || name_size >= METADATA_NAME_SIZE || document->compression != COMPRESSION_LZ4 || document->chunk_count != document->file_size / METADATA_CHUNK_SIZE + (document->file_size % METADATA_CHUNK_SIZE != 0U) || capacity < 1U + DOCUMENT_FIXED_SIZE + name_size)
    {
        errno = EINVAL;
        return -1;
    }
    output[offset++] = operation;
    memcpy(output + offset, document->id.bytes, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    wire_put_u64(output + offset, document->file_size);
    offset += 8U;
    wire_put_u64(output + offset, document->chunk_count);
    offset += 8U;
    output[offset++] = document->compression;
    wire_put_u16(output + offset, (uint16_t)name_size);
    offset += 2U;
    memcpy(output + offset, document->name, name_size);
    offset += name_size;
    *used = offset;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-decode-document-at"></a>

#### decode_document_at

Fonte: `transfer_protocol.c:102` (linha nesta revisão; pode mudar em futuras edições).

```c
static int decode_document_at(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document, size_t *used);
```

faz a leitura inversa com checagens de tamanho, operação, nome, compressão e contagem de chunks. Rejeita NUL ou barra dentro do nome transmitido.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `uint8_t expected_operation`: Suboperação que o decoder exige encontrar.
- `TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `size_t *used`: Ponteiro para a quantidade de bytes consumidos/escritos.

**Funções do projeto usadas diretamente:** [`wire_get_u16` (wire.h)](#fn-wire-h-wire-get-u16); [`wire_get_u64` (wire.h)](#fn-wire-h-wire-get-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`transfer_decode_document` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-document); [`transfer_decode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-result); [`transfer_decode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-announcement).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de decode_document_at, para acompanhar a explicação</summary>

```c
static int decode_document_at(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document, size_t *used)
{
    size_t offset = 0U;
    uint16_t name_size;

    if (payload == NULL || document == NULL || used == NULL || payload_size < 1U + DOCUMENT_FIXED_SIZE || payload[offset++] != expected_operation)
    {
        errno = EBADMSG;
        return -1;
    }
    memset(document, 0, sizeof(*document));
    memcpy(document->id.bytes, payload + offset, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    document->file_size = wire_get_u64(payload + offset);
    offset += 8U;
    document->chunk_count = wire_get_u64(payload + offset);
    offset += 8U;
    document->compression = payload[offset++];
    name_size = wire_get_u16(payload + offset);
    offset += 2U;
    if (name_size == 0U || name_size >= METADATA_NAME_SIZE || offset + name_size > payload_size || memchr(payload + offset, '\0', name_size) != NULL || memchr(payload + offset, '/', name_size) != NULL || document->compression != COMPRESSION_LZ4 || document->chunk_count != document->file_size / METADATA_CHUNK_SIZE + (document->file_size % METADATA_CHUNK_SIZE != 0U))
    {
        errno = EBADMSG;
        return -1;
    }
    memcpy(document->name, payload + offset, name_size);
    document->name[name_size] = '\0';
    offset += name_size;
    *used = offset;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-fill-transaction-id"></a>

#### transfer_fill_transaction_id

Fonte: `transfer_protocol.c:134` (linha nesta revisão; pode mudar em futuras edições).

```c
void transfer_fill_transaction_id(uint8_t output[TRANSACTION_ID_SIZE], const uint8_t source_node[NODE_ID_SIZE]);
```

Compõe timestamp de nanossegundos com incremento lógico atômico, prefixo do NodeID e sequência atômica. Evita reutilização dentro da instância mesmo se o relógio retroceder. Respostas copiam o ID original.

**Parâmetros**

- `uint8_t output[TRANSACTION_ID_SIZE]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.
- `const uint8_t source_node[NODE_ID_SIZE]`: 32 bytes da origem usados no header ou na composição do TransactionID.

**Passo a passo**

1. Obtém timestamp UTC em nanossegundos e lê o último valor lógico atômico.
2. Se o relógio não avançou, usa previous+1. Compare-exchange tenta publicar esse valor; disputa entre threads exige repetir.
3. Obtém também uma sequência atômica e a escreve nos últimos quatro bytes.
4. O resultado tem 8 bytes de tempo lógico, 4 bytes do prefixo do NodeID (ou zeros) e 4 bytes de sequência.
5. Não é UUID aleatório nem autenticação. A finalidade é correlação sem reutilização dentro da instância, não consenso entre máquinas.

**Funções do projeto usadas diretamente:** [`wire_put_u32` (wire.h)](#fn-wire-h-wire-put-u32); [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`rpc_call` (rpc.c)](#fn-rpc-c-rpc-call); [`connect_and_join` (superpeer_app.c)](#fn-superpeer-app-c-connect-and-join); [`execute_command` (superpeer_app.c)](#fn-superpeer-app-c-execute-command).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de transfer_fill_transaction_id, para acompanhar a explicação</summary>

```c
void transfer_fill_transaction_id(uint8_t output[TRANSACTION_ID_SIZE], const uint8_t source_node[NODE_ID_SIZE])
{
    struct timespec now;
    (void)timespec_get(&now, TIME_UTC);
    uint64_t timestamp = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    uint_fast64_t previous = atomic_load_explicit(&transaction_clock, memory_order_relaxed);
    for (;;)
    {
        if (timestamp <= previous) timestamp = previous + 1U;
        if (atomic_compare_exchange_weak_explicit(&transaction_clock, &previous, timestamp, memory_order_relaxed, memory_order_relaxed)) break;
    }
    uint32_t sequence = atomic_fetch_add_explicit(&transaction_sequence, 1U, memory_order_relaxed) + 1U;

    wire_put_u64(output, timestamp);
    if (source_node == NULL)
    {
        memset(output + 8U, 0, 4U);
    }
    else
    {
        memcpy(output + 8U, source_node, 4U);
    }
    wire_put_u32(output + 12U, sequence);
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-document"></a>

#### transfer_encode_document

Fonte: `transfer_protocol.c:159` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_document(const TransferDocument *document, uint8_t **output, uint32_t *output_size, uint8_t operation);
```

Calcula o tamanho do registro básico de documento, aloca o buffer e delega a encode_document_at. Devolve o payload por output e seu tamanho por output_size; libera o buffer em falha.

**Parâmetros**

- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *output_size`: Ponteiro que recebe o tamanho produzido.
- `uint8_t operation`: Suboperação do payload; diferente do tipo do header.

**Funções do projeto usadas diretamente:** [`document_wire_size` (transfer_protocol.c)](#fn-transfer-protocol-c-document-wire-size); [`encode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-document-at).

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EOVERFLOW`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_document, para acompanhar a explicação</summary>

```c
int transfer_encode_document(const TransferDocument *document, uint8_t **output, uint32_t *output_size, uint8_t operation)
{
    size_t size;
    size_t used;
    uint8_t *buffer;

    if (document == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    size = document_wire_size(document);
    if (size > MAX_PAYLOAD_SIZE || size > UINT32_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL)
    {
        return -1;
    }
    if (encode_document_at(document, operation, buffer, size, &used) < 0)
    {
        free(buffer);
        return -1;
    }
    *output = buffer;
    *output_size = (uint32_t)used;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-document"></a>

#### transfer_decode_document

Fonte: `transfer_protocol.c:192` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_document(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document);
```

Delega a decode_document_at e exige que used corresponda ao tamanho total recebido. Assim rejeita bytes extras depois do documento. Não aloca o conteúdo do arquivo: preenche somente seus metadados.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `uint8_t expected_operation`: Suboperação que o decoder exige encontrar.
- `TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.

**Funções do projeto usadas diretamente:** [`decode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-document-at).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store).

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_decode_document, para acompanhar a explicação</summary>

```c
int transfer_decode_document(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document)
{
    size_t used;

    if (decode_document_at(payload, payload_size, expected_operation, document, &used) < 0 || used != payload_size)
    {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-chunk"></a>

#### transfer_encode_chunk

Fonte: `transfer_protocol.c:204` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_chunk(const TransferChunk *chunk, uint8_t operation, uint8_t **output, uint32_t *output_size);
```

Valida o descritor, tamanhos e ponteiro de dados; aloca e escreve operação, ObjectID, índice, offset, tamanhos, hash e bytes comprimidos. Faz cópia do conteúdo LZ4 para o payload. O buffer codificado pertence ao chamador.

**Parâmetros**

- `const TransferChunk *chunk`: Descritor e, quando TransferChunk, ponteiro para conteúdo comprimido.
- `uint8_t operation`: Suboperação do payload; diferente do tipo do header.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *output_size`: Ponteiro que recebe o tamanho produzido.

**Funções do projeto usadas diretamente:** [`wire_put_u32` (wire.h)](#fn-wire-h-wire-put-u32); [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`upload_worker` (file_client.c)](#fn-file-client-c-upload-worker); [`handle_download` (peer_service.c)](#fn-peer-service-c-handle-download).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EOVERFLOW`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_chunk, para acompanhar a explicação</summary>

```c
int transfer_encode_chunk(const TransferChunk *chunk, uint8_t operation, uint8_t **output, uint32_t *output_size)
{
    size_t size;
    size_t offset = 0U;
    uint8_t *buffer;

    if (chunk == NULL || output == NULL || output_size == NULL || chunk->data == NULL || chunk->raw_size == 0U || chunk->compressed_size == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    size = CHUNK_FIXED_SIZE + chunk->compressed_size;
    if (size > MAX_PAYLOAD_SIZE || size > UINT32_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[offset++] = operation;
    memcpy(buffer + offset, chunk->id.bytes, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    wire_put_u64(buffer + offset, chunk->index);
    offset += 8U;
    wire_put_u64(buffer + offset, chunk->offset);
    offset += 8U;
    wire_put_u32(buffer + offset, chunk->raw_size);
    offset += 4U;
    wire_put_u32(buffer + offset, chunk->compressed_size);
    offset += 4U;
    memcpy(buffer + offset, chunk->hash, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    memcpy(buffer + offset, chunk->data, chunk->compressed_size);
    *output = buffer;
    *output_size = (uint32_t)size;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-chunk"></a>

#### transfer_decode_chunk

Fonte: `transfer_protocol.c:245` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_chunk(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferChunk *chunk);
```

Lê os campos fixos, confere operação e comprimento total e preenche TransferChunk. O campo data aponta para dentro do payload recebido, sem cópia. Não valida o SHA-256 por si só; a camada que descomprime precisa fazê-lo.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `uint8_t expected_operation`: Suboperação que o decoder exige encontrar.
- `TransferChunk *chunk`: Descritor e, quando TransferChunk, ponteiro para conteúdo comprimido.

**Funções do projeto usadas diretamente:** [`wire_get_u32` (wire.h)](#fn-wire-h-wire-get-u32); [`wire_get_u64` (wire.h)](#fn-wire-h-wire-get-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`download_one` (file_client.c)](#fn-file-client-c-download-one); [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_decode_chunk, para acompanhar a explicação</summary>

```c
int transfer_decode_chunk(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferChunk *chunk)
{
    size_t offset = 0U;

    if (payload == NULL || chunk == NULL || payload_size < CHUNK_FIXED_SIZE || payload[offset++] != expected_operation)
    {
        errno = EBADMSG;
        return -1;
    }
    memset(chunk, 0, sizeof(*chunk));
    memcpy(chunk->id.bytes, payload + offset, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    chunk->index = wire_get_u64(payload + offset);
    offset += 8U;
    chunk->offset = wire_get_u64(payload + offset);
    offset += 8U;
    chunk->raw_size = wire_get_u32(payload + offset);
    offset += 4U;
    chunk->compressed_size = wire_get_u32(payload + offset);
    offset += 4U;
    memcpy(chunk->hash, payload + offset, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    if (chunk->raw_size == 0U || chunk->raw_size > METADATA_CHUNK_SIZE || chunk->compressed_size == 0U || offset + chunk->compressed_size != payload_size)
    {
        errno = EBADMSG;
        return -1;
    }
    chunk->data = payload + offset;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-object-operation"></a>

#### transfer_encode_object_operation

Fonte: `transfer_protocol.c:276` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_object_operation(uint8_t operation, const ObjectID *id, uint8_t **output, uint32_t *output_size);
```

Aloca o formato curto de uma operação seguida dos 32 bytes de ObjectID. É usado para COMMIT do upload, sem nome, descritores ou conteúdo. O chamador libera o payload.

**Parâmetros**

- `uint8_t operation`: Suboperação do payload; diferente do tipo do header.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *output_size`: Ponteiro que recebe o tamanho produzido.

**Chamadores diretos encontrados nos fontes inventariados:** [`file_client_upload` (file_client.c)](#fn-file-client-c-file-client-upload).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_object_operation, para acompanhar a explicação</summary>

```c
int transfer_encode_object_operation(uint8_t operation, const ObjectID *id, uint8_t **output, uint32_t *output_size)
{
    uint8_t *buffer;

    if (id == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    buffer = malloc(1U + OBJECT_ID_SIZE);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[0] = operation;
    memcpy(buffer + 1U, id->bytes, OBJECT_ID_SIZE);
    *output = buffer;
    *output_size = 1U + OBJECT_ID_SIZE;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-object-operation"></a>

#### transfer_decode_object_operation

Fonte: `transfer_protocol.c:297` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_object_operation(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, ObjectID *id);
```

Exige o tamanho exato do formato curto e a operação esperada, então copia os 32 bytes para o ObjectID de saída. Não procura o objeto em disco nem no índice.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `uint8_t expected_operation`: Suboperação que o decoder exige encontrar.
- `ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_store` (peer_service.c)](#fn-peer-service-c-handle-store).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_decode_object_operation, para acompanhar a explicação</summary>

```c
int transfer_decode_object_operation(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, ObjectID *id)
{
    if (payload == NULL || id == NULL || payload_size != 1U + OBJECT_ID_SIZE || payload[0] != expected_operation)
    {
        errno = EBADMSG;
        return -1;
    }
    memcpy(id->bytes, payload + 1U, OBJECT_ID_SIZE);
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-lookup-request"></a>

#### transfer_encode_lookup_request

Fonte: `transfer_protocol.c:308` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_lookup_request(const char *selector, uint8_t **output, uint32_t *output_size);
```

se o texto for um ObjectID hexadecimal válido, envia seletor binário por ID; caso contrário, envia nome. Aloca o payload.

**Parâmetros**

- `const char *selector`: Nome de arquivo ou ObjectID hexadecimal que deve ser resolvido.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *output_size`: Ponteiro que recebe o tamanho produzido.

**Funções do projeto usadas diretamente:** [`object_id_from_hex` (transfer_protocol.c)](#fn-transfer-protocol-c-object-id-from-hex); [`wire_put_u16` (wire.h)](#fn-wire-h-wire-put-u16).

**Chamadores diretos encontrados nos fontes inventariados:** [`lookup_document` (file_client.c)](#fn-file-client-c-lookup-document).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_lookup_request, para acompanhar a explicação</summary>

```c
int transfer_encode_lookup_request(const char *selector, uint8_t **output, uint32_t *output_size)
{
    ObjectID id;
    size_t length;
    uint8_t type;
    uint8_t *buffer;

    if (selector == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    length = strlen(selector);
    type = object_id_from_hex(&id, selector) == 0 ? TRANSFER_SELECTOR_OBJECT_ID : TRANSFER_SELECTOR_NAME;
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        length = OBJECT_ID_SIZE;
    }
    else if (length == 0U || length >= METADATA_NAME_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    buffer = malloc(3U + length);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[0] = type;
    wire_put_u16(buffer + 1U, (uint16_t)length);
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        memcpy(buffer + 3U, id.bytes, OBJECT_ID_SIZE);
    }
    else
    {
        memcpy(buffer + 3U, selector, length);
    }
    *output = buffer;
    *output_size = (uint32_t)(3U + length);
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-lookup-request"></a>

#### transfer_decode_lookup_request

Fonte: `transfer_protocol.c:351` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_lookup_request(const uint8_t *payload, size_t payload_size, TransferSelectorType *type, ObjectID *id, char name[METADATA_NAME_SIZE]);
```

lê tipo e comprimento do seletor; valida comprimento, ausência de barra/NUL no nome e separa ObjectID ou nome.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `TransferSelectorType *type`: Código de mensagem/seletor, de acordo com o enum da assinatura.
- `ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `char name[METADATA_NAME_SIZE]`: Nome textual usado como chave, metadado ou variável de ambiente, conforme a finalidade descrita.

**Funções do projeto usadas diretamente:** [`wire_get_u16` (wire.h)](#fn-wire-h-wire-get-u16).

**Chamadores diretos encontrados nos fontes inventariados:** [`answer_lookup` (superpeer_app.c)](#fn-superpeer-app-c-answer-lookup).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_decode_lookup_request, para acompanhar a explicação</summary>

```c
int transfer_decode_lookup_request(const uint8_t *payload, size_t payload_size, TransferSelectorType *type, ObjectID *id, char name[METADATA_NAME_SIZE])
{
    uint16_t length;

    if (payload == NULL || type == NULL || id == NULL || name == NULL || payload_size < 3U)
    {
        errno = EBADMSG;
        return -1;
    }
    length = wire_get_u16(payload + 1U);
    if ((payload[0] != TRANSFER_SELECTOR_OBJECT_ID && payload[0] != TRANSFER_SELECTOR_NAME) || 3U + length != payload_size)
    {
        errno = EBADMSG;
        return -1;
    }
    memset(id, 0, sizeof(*id));
    memset(name, 0, METADATA_NAME_SIZE);
    if (payload[0] == TRANSFER_SELECTOR_OBJECT_ID)
    {
        if (length != OBJECT_ID_SIZE)
        {
            errno = EBADMSG;
            return -1;
        }
        memcpy(id->bytes, payload + 3U, OBJECT_ID_SIZE);
    }
    else
    {
        if (length == 0U || length >= METADATA_NAME_SIZE || memchr(payload + 3U, '\0', length) != NULL || memchr(payload + 3U, '/', length) != NULL)
        {
            errno = EBADMSG;
            return -1;
        }
        memcpy(name, payload + 3U, length);
    }
    *type = (TransferSelectorType)payload[0];
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-lookup-result-free"></a>

#### transfer_lookup_result_free

Fonte: `transfer_protocol.c:390` (linha nesta revisão; pode mudar em futuras edições).

```c
void transfer_lookup_result_free(TransferLookupResult *result);
```

libera listas de endpoints de cada chunk e o vetor de chunks; zera a estrutura. Deve ser chamado para um resultado de lookup montado ou decodificado.

**Parâmetros**

- `TransferLookupResult *result`: Estrutura de resultado de lookup, com documento e vetor de localizações; liberar por transfer_lookup_result_free.

**Chamadores diretos encontrados nos fontes inventariados:** [`directory_lookup` (directory.c)](#fn-directory-c-directory-lookup); [`file_client_download` (file_client.c)](#fn-file-client-c-file-client-download); [`answer_lookup` (superpeer_app.c)](#fn-superpeer-app-c-answer-lookup); [`transfer_decode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-result).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de transfer_lookup_result_free, para acompanhar a explicação</summary>

```c
void transfer_lookup_result_free(TransferLookupResult *result)
{
    uint64_t index;

    if (result == NULL)
    {
        return;
    }
    if (result->chunks != NULL)
    {
        for (index = 0U; index < result->document.chunk_count; ++index)
        {
            free(result->chunks[index].peers);
        }
    }
    free(result->chunks);
    memset(result, 0, sizeof(*result));
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-lookup-result"></a>

#### transfer_encode_lookup_result

Fonte: `transfer_protocol.c:409` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_lookup_result(const TransferLookupResult *result, uint8_t **output, uint32_t *output_size);
```

serializa documento e, para cada chunk, descritor de 56 bytes, quantidade de Peers e seus NodeIDs, IPs e portas. Calcula tamanho e rejeita excesso antes de alocar.

**Parâmetros**

- `const TransferLookupResult *result`: Estrutura de resultado de lookup, com documento e vetor de localizações; liberar por transfer_lookup_result_free.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *output_size`: Ponteiro que recebe o tamanho produzido.

**Funções do projeto usadas diretamente:** [`bounded_string_length` (transfer_protocol.c)](#fn-transfer-protocol-c-bounded-string-length); [`document_wire_size` (transfer_protocol.c)](#fn-transfer-protocol-c-document-wire-size); [`encode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-document-at); [`encode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-descriptor); [`wire_put_u16` (wire.h)](#fn-wire-h-wire-put-u16).

**Chamadores diretos encontrados nos fontes inventariados:** [`answer_lookup` (superpeer_app.c)](#fn-superpeer-app-c-answer-lookup).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EOVERFLOW`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_lookup_result, para acompanhar a explicação</summary>

```c
int transfer_encode_lookup_result(const TransferLookupResult *result, uint8_t **output, uint32_t *output_size)
{
    size_t size;
    size_t used;
    size_t offset;
    uint64_t chunk_index;
    uint8_t *buffer;

    if (result == NULL || output == NULL || output_size == NULL || (result->document.chunk_count != 0U && result->chunks == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    size = document_wire_size(&result->document);
    for (chunk_index = 0U; chunk_index < result->document.chunk_count; ++chunk_index)
    {
        size_t peer_index;

        if (result->chunks[chunk_index].peer_count > UINT16_MAX)
        {
            errno = EOVERFLOW;
            return -1;
        }
        if (size > SIZE_MAX - 2U - DESCRIPTOR_SIZE)
        {
            errno = EOVERFLOW;
            return -1;
        }
        size += 2U + DESCRIPTOR_SIZE;
        for (peer_index = 0U; peer_index < result->chunks[chunk_index].peer_count; ++peer_index)
        {
            size_t ip_size = bounded_string_length(result->chunks[chunk_index].peers[peer_index].ip, NODE_ADDRESS_SIZE);

            if (ip_size == 0U || ip_size >= NODE_ADDRESS_SIZE || size > SIZE_MAX - ENDPOINT_FIXED_SIZE - ip_size)
            {
                errno = EINVAL;
                return -1;
            }
            size += ENDPOINT_FIXED_SIZE + ip_size;
        }
    }
    if (size > MAX_PAYLOAD_SIZE || size > UINT32_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL)
    {
        return -1;
    }
    if (encode_document_at(&result->document, TRANSFER_DOWNLOAD_METADATA, buffer, size, &used) < 0)
    {
        free(buffer);
        return -1;
    }
    offset = used;
    for (chunk_index = 0U; chunk_index < result->document.chunk_count; ++chunk_index)
    {
        size_t peer_index;

        encode_descriptor(buffer + offset, &result->chunks[chunk_index].descriptor);
        offset += DESCRIPTOR_SIZE;
        wire_put_u16(buffer + offset, (uint16_t)result->chunks[chunk_index].peer_count);
        offset += 2U;
        for (peer_index = 0U; peer_index < result->chunks[chunk_index].peer_count; ++peer_index)
        {
            const TransferEndpoint *endpoint = &result->chunks[chunk_index].peers[peer_index];
            size_t ip_size = strlen(endpoint->ip);

            memcpy(buffer + offset, endpoint->node_id.bytes, NODE_ID_SIZE);
            offset += NODE_ID_SIZE;
            buffer[offset++] = (uint8_t)ip_size;
            memcpy(buffer + offset, endpoint->ip, ip_size);
            offset += ip_size;
            wire_put_u16(buffer + offset, endpoint->port);
            offset += 2U;
        }
    }
    *output = buffer;
    *output_size = (uint32_t)offset;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-lookup-result"></a>

#### transfer_decode_lookup_result

Fonte: `transfer_protocol.c:493` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_lookup_result(const uint8_t *payload, size_t payload_size, TransferLookupResult *result);
```

reconstrói documento e listas de endpoints; valida limites, portas e ausência de bytes sobrando. Em erro libera as partes já alocadas.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `TransferLookupResult *result`: Estrutura de resultado de lookup, com documento e vetor de localizações; liberar por transfer_lookup_result_free.

**Funções do projeto usadas diretamente:** [`decode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-document-at); [`transfer_lookup_result_free` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-lookup-result-free); [`decode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-descriptor); [`wire_get_u16` (wire.h)](#fn-wire-h-wire-get-u16).

**Chamadores diretos encontrados nos fontes inventariados:** [`lookup_document` (file_client.c)](#fn-file-client-c-lookup-document).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

**Limpeza centralizada:** os saltos para cleanup/failure/invalid reúnem a saída em erro. Leia quais recursos já foram obtidos antes de cada salto; nem toda falha acontece no mesmo estágio.

<details>
<summary>Código completo de transfer_decode_lookup_result, para acompanhar a explicação</summary>

```c
int transfer_decode_lookup_result(const uint8_t *payload, size_t payload_size, TransferLookupResult *result)
{
    size_t offset;
    uint64_t chunk_index;

    if (payload == NULL || result == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (decode_document_at(payload, payload_size, TRANSFER_DOWNLOAD_METADATA, &result->document, &offset) < 0 || result->document.chunk_count > SIZE_MAX / sizeof(*result->chunks))
    {
        return -1;
    }
    if (result->document.chunk_count > (payload_size - offset) / (DESCRIPTOR_SIZE + 2U))
    {
        errno = EBADMSG;
        return -1;
    }
    if (result->document.chunk_count != 0U)
    {
        result->chunks = calloc((size_t)result->document.chunk_count, sizeof(*result->chunks));
        if (result->chunks == NULL)
        {
            return -1;
        }
    }
    for (chunk_index = 0U; chunk_index < result->document.chunk_count; ++chunk_index)
    {
        uint16_t peer_count;
        size_t peer_index;

        if (offset + DESCRIPTOR_SIZE + 2U > payload_size)
        {
            goto invalid;
        }
        decode_descriptor(payload + offset, &result->chunks[chunk_index].descriptor);
        if (result->chunks[chunk_index].descriptor.index != chunk_index) goto invalid;
        offset += DESCRIPTOR_SIZE;
        peer_count = wire_get_u16(payload + offset);
        offset += 2U;
        result->chunks[chunk_index].peer_count = peer_count;
        if (peer_count != 0U)
        {
            result->chunks[chunk_index].peers = calloc(peer_count, sizeof(*result->chunks[chunk_index].peers));
            if (result->chunks[chunk_index].peers == NULL)
            {
                transfer_lookup_result_free(result);
                return -1;
            }
        }
        for (peer_index = 0U; peer_index < peer_count; ++peer_index)
        {
            TransferEndpoint *endpoint = &result->chunks[chunk_index].peers[peer_index];
            uint8_t ip_size;

            if (offset + ENDPOINT_FIXED_SIZE > payload_size)
            {
                goto invalid;
            }
            memcpy(endpoint->node_id.bytes, payload + offset, NODE_ID_SIZE);
            offset += NODE_ID_SIZE;
            ip_size = payload[offset++];
            if (ip_size == 0U || ip_size >= NODE_ADDRESS_SIZE || offset + ip_size + 2U > payload_size)
            {
                goto invalid;
            }
            memcpy(endpoint->ip, payload + offset, ip_size);
            endpoint->ip[ip_size] = '\0';
            offset += ip_size;
            endpoint->port = wire_get_u16(payload + offset);
            offset += 2U;
            if (endpoint->port == 0U)
            {
                goto invalid;
            }
        }
    }
    if (offset != payload_size)
    {
        goto invalid;
    }
    return 0;

invalid:
    transfer_lookup_result_free(result);
    errno = EBADMSG;
    return -1;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-chunk-request"></a>

#### transfer_encode_chunk_request

Fonte: `transfer_protocol.c:584` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_chunk_request(const ObjectID *id, uint64_t index, uint8_t **output, uint32_t *output_size);
```

cria DOWNLOAD_REQ com operação, ObjectID e índice do chunk.

**Parâmetros**

- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *output_size`: Ponteiro que recebe o tamanho produzido.

**Funções do projeto usadas diretamente:** [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`download_one` (file_client.c)](#fn-file-client-c-download-one).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_chunk_request, para acompanhar a explicação</summary>

```c
int transfer_encode_chunk_request(const ObjectID *id, uint64_t index, uint8_t **output, uint32_t *output_size)
{
    uint8_t *buffer;

    if (id == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    buffer = malloc(1U + OBJECT_ID_SIZE + 8U);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[0] = TRANSFER_DOWNLOAD_CHUNK;
    memcpy(buffer + 1U, id->bytes, OBJECT_ID_SIZE);
    wire_put_u64(buffer + 1U + OBJECT_ID_SIZE, index);
    *output = buffer;
    *output_size = 1U + OBJECT_ID_SIZE + 8U;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-chunk-request"></a>

#### transfer_decode_chunk_request

Fonte: `transfer_protocol.c:606` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_chunk_request(const uint8_t *payload, size_t payload_size, ObjectID *id, uint64_t *index);
```

exige tamanho e operação exatos e recupera ObjectID e índice.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t payload_size`: Quantidade exata de bytes do corpo recebido ou a enviar.
- `ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.
- `uint64_t *index`: Índice iniciado em zero; se ponteiro, posição encontrada a devolver.

**Funções do projeto usadas diretamente:** [`wire_get_u64` (wire.h)](#fn-wire-h-wire-get-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`handle_download` (peer_service.c)](#fn-peer-service-c-handle-download).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_decode_chunk_request, para acompanhar a explicação</summary>

```c
int transfer_decode_chunk_request(const uint8_t *payload, size_t payload_size, ObjectID *id, uint64_t *index)
{
    if (payload == NULL || id == NULL || index == NULL || payload_size != 1U + OBJECT_ID_SIZE + 8U || payload[0] != TRANSFER_DOWNLOAD_CHUNK)
    {
        errno = EBADMSG;
        return -1;
    }
    memcpy(id->bytes, payload + 1U, OBJECT_ID_SIZE);
    *index = wire_get_u64(payload + 1U + OBJECT_ID_SIZE);
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-encode-descriptor"></a>

#### encode_descriptor

Fonte: `transfer_protocol.c:618` (linha nesta revisão; pode mudar em futuras edições).

```c
static void encode_descriptor(uint8_t *output, const MetadataChunk *chunk);
```

Escreve um MetadataChunk em 56 bytes: índice 8, offset 8, tamanho original 4, tamanho comprimido 4 e SHA-256 32. Não inclui ponteiros nem padding e não aloca. O chamador garante espaço e validade dos endereços.

**Parâmetros**

- `uint8_t *output`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.
- `const MetadataChunk *chunk`: Descritor e, quando TransferChunk, ponteiro para conteúdo comprimido.

**Funções do projeto usadas diretamente:** [`wire_put_u32` (wire.h)](#fn-wire-h-wire-put-u32); [`wire_put_u64` (wire.h)](#fn-wire-h-wire-put-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`transfer_encode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-result); [`transfer_encode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-announcement).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de encode_descriptor, para acompanhar a explicação</summary>

```c
static void encode_descriptor(uint8_t *output, const MetadataChunk *chunk)
{
    wire_put_u64(output, chunk->index);
    wire_put_u64(output + 8U, chunk->offset);
    wire_put_u32(output + 16U, chunk->raw_size);
    wire_put_u32(output + 20U, chunk->compressed_size);
    memcpy(output + 24U, chunk->hash, OBJECT_ID_SIZE);
}
```

</details>

<a id="fn-transfer-protocol-c-decode-descriptor"></a>

#### decode_descriptor

Fonte: `transfer_protocol.c:627` (linha nesta revisão; pode mudar em futuras edições).

```c
static void decode_descriptor(const uint8_t *input, MetadataChunk *chunk);
```

Lê os mesmos 56 bytes e preenche os campos do descritor. Apenas converte a representação; validar índice, offset, hash e limites semânticos continua sendo responsabilidade do chamador.

**Parâmetros**

- `const uint8_t *input`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `MetadataChunk *chunk`: Descritor e, quando TransferChunk, ponteiro para conteúdo comprimido.

**Funções do projeto usadas diretamente:** [`wire_get_u32` (wire.h)](#fn-wire-h-wire-get-u32); [`wire_get_u64` (wire.h)](#fn-wire-h-wire-get-u64).

**Chamadores diretos encontrados nos fontes inventariados:** [`transfer_decode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-result); [`transfer_decode_announcement` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-announcement).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de decode_descriptor, para acompanhar a explicação</summary>

```c
static void decode_descriptor(const uint8_t *input, MetadataChunk *chunk)
{
    chunk->index = wire_get_u64(input);
    chunk->offset = wire_get_u64(input + 8U);
    chunk->raw_size = wire_get_u32(input + 16U);
    chunk->compressed_size = wire_get_u32(input + 20U);
    memcpy(chunk->hash, input + 24U, OBJECT_ID_SIZE);
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-encode-announcement"></a>

#### transfer_encode_announcement

Fonte: `transfer_protocol.c:636` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_encode_announcement(const TransferDocument *document, const MetadataChunk *chunks, uint8_t **output, uint32_t *size);
```

Codifica o documento com a operação de anúncio v2 e acrescenta um descritor de 56 bytes para cada chunk. Verifica limites de tamanho/overflow, aloca o payload e só publica output/size após concluir. O chamador libera o buffer.

**Parâmetros**

- `const TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `const MetadataChunk *chunks`: Vetor de descritores/localizações; quantidade vem do documento associado.
- `uint8_t **output`: Endereço da variável que recebe um ponteiro produzido; confira a seção de ownership do módulo para destruição.
- `uint32_t *size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.

**Funções do projeto usadas diretamente:** [`document_wire_size` (transfer_protocol.c)](#fn-transfer-protocol-c-document-wire-size); [`encode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-document-at); [`encode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-descriptor).

**Chamadores diretos encontrados nos fontes inventariados:** [`announce_document` (peer_service.c)](#fn-peer-service-c-announce-document).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EMSGSIZE`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_encode_announcement, para acompanhar a explicação</summary>

```c
int transfer_encode_announcement(const TransferDocument *document, const MetadataChunk *chunks, uint8_t **output, uint32_t *size)
{
    if (document == NULL || chunks == NULL || output == NULL || size == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    size_t base = document_wire_size(document), used;
    if (base > MAX_PAYLOAD_SIZE || document->chunk_count > (MAX_PAYLOAD_SIZE - base) / DESCRIPTOR_SIZE) { errno = EMSGSIZE; return -1; }
    size_t total = base + (size_t)document->chunk_count * DESCRIPTOR_SIZE;
    uint8_t *buffer = malloc(total);
    if (buffer == NULL) return -1;
    if (encode_document_at(document, TRANSFER_STORE_ANNOUNCE, buffer, total, &used) < 0) { free(buffer); return -1; }
    for (uint64_t i = 0U; i < document->chunk_count; ++i) encode_descriptor(buffer + used + (size_t)i * DESCRIPTOR_SIZE, &chunks[i]);
    *output = buffer;
    *size = (uint32_t)total;
    return 0;
}
```

</details>

<a id="fn-transfer-protocol-c-transfer-decode-announcement"></a>

#### transfer_decode_announcement

Fonte: `transfer_protocol.c:652` (linha nesta revisão; pode mudar em futuras edições).

```c
int transfer_decode_announcement(const uint8_t *payload, size_t size, TransferDocument *document, MetadataChunk **chunks);
```

Decodifica o documento do anúncio v2, exige que o restante contenha exatamente a quantidade esperada de descritores e aloca sua matriz. Não recebe bytes LZ4. O chamador libera a matriz; o registro atômico e a validação de descritores pertencem ao módulo de metadados.

**Parâmetros**

- `const uint8_t *payload`: Região de bytes; o comprimento vem do parâmetro correspondente. const impede alteração por este ponteiro, mas não determina ownership.
- `size_t size`: Quantidade de bytes, salvo se o tipo for ponteiro de saída para essa quantidade.
- `TransferDocument *document`: Metadados do documento; não contém o PDF inteiro.
- `MetadataChunk **chunks`: Vetor de descritores/localizações; quantidade vem do documento associado.

**Funções do projeto usadas diretamente:** [`decode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-document-at); [`decode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-descriptor).

**Chamadores diretos encontrados nos fontes inventariados:** [`register_announcement` (superpeer_app.c)](#fn-superpeer-app-c-register-announcement).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.

**Retorno:** as expressões presentes nesta função são `-1`, `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

**Erros explícitos neste corpo:** `EINVAL`, `EBADMSG`. Outros erros podem vir das funções chamadas; a lista não é exaustiva para a operação inteira.

<details>
<summary>Código completo de transfer_decode_announcement, para acompanhar a explicação</summary>

```c
int transfer_decode_announcement(const uint8_t *payload, size_t size, TransferDocument *document, MetadataChunk **chunks)
{
    size_t used;
    if (chunks == NULL || document == NULL) { errno = EINVAL; return -1; }
    *chunks = NULL;
    if (decode_document_at(payload, size, TRANSFER_STORE_ANNOUNCE, document, &used) < 0) return -1;
    if (document->chunk_count == 0U || document->chunk_count > (size - used) / DESCRIPTOR_SIZE || size - used != document->chunk_count * DESCRIPTOR_SIZE || document->chunk_count > SIZE_MAX / sizeof(**chunks)) { errno = EBADMSG; return -1; }
    MetadataChunk *list = calloc((size_t)document->chunk_count, sizeof(*list));
    if (list == NULL) return -1;
    for (uint64_t i = 0U; i < document->chunk_count; ++i) decode_descriptor(payload + used + (size_t)i * DESCRIPTOR_SIZE, &list[i]);
    *chunks = list;
    return 0;
}
```

</details>

<a id="mod-wire-h"></a>

### wire.h

[wire_put_u16](#fn-wire-h-wire-put-u16) · [wire_get_u16](#fn-wire-h-wire-get-u16) · [wire_put_u32](#fn-wire-h-wire-put-u32) · [wire_get_u32](#fn-wire-h-wire-get-u32) · [wire_put_u64](#fn-wire-h-wire-put-u64) · [wire_get_u64](#fn-wire-h-wire-get-u64)

<a id="fn-wire-h-wire-put-u16"></a>

#### wire_put_u16

Fonte: `wire.h:5` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline void wire_put_u16(uint8_t *destination, uint16_t value);
```

Escreve um inteiro unsigned de 16 bits em 2 bytes big-endian. Deslocamentos descartam os bits menos significativos temporariamente, e o cast para uint8_t seleciona o byte. Não aloca, não envia pela rede e não confere capacidade: destination deve apontar para pelo menos 2 bytes graváveis.

**Parâmetros**

- `uint8_t *destination`: Buffer onde a representação em bytes será escrita.
- `uint16_t value`: Inteiro unsigned a serializar.

**Chamadores diretos encontrados nos fontes inventariados:** [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`encode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-document-at); [`transfer_encode_lookup_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-request); [`transfer_encode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-lookup-result).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de wire_put_u16, para acompanhar a explicação</summary>

```c
static inline void wire_put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}
```

</details>

<a id="fn-wire-h-wire-get-u16"></a>

#### wire_get_u16

Fonte: `wire.h:11` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline uint16_t wire_get_u16(const uint8_t *source);
```

Reconstrói um inteiro unsigned de 16 bits a partir de 2 bytes big-endian. Cada byte é colocado na posição correspondente e combinado por OR. Não altera source nem valida seu comprimento: o chamador precisa garantir 2 bytes legíveis. Retorna o número, não um status de sucesso.

**Parâmetros**

- `const uint8_t *source`: Buffer de entrada emprestado, contendo os bytes a interpretar.

**Chamadores diretos encontrados nos fontes inventariados:** [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`decode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-document-at); [`transfer_decode_lookup_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-request); [`transfer_decode_lookup_result` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-lookup-result).

**Retorno:** as expressões presentes nesta função são `(uint16_t)(((uint16_t)source[0] << 8) | source[1])`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de wire_get_u16, para acompanhar a explicação</summary>

```c
static inline uint16_t wire_get_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}
```

</details>

<a id="fn-wire-h-wire-put-u32"></a>

#### wire_put_u32

Fonte: `wire.h:16` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline void wire_put_u32(uint8_t *destination, uint32_t value);
```

Escreve um inteiro unsigned de 32 bits em 4 bytes big-endian. Deslocamentos descartam os bits menos significativos temporariamente, e o cast para uint8_t seleciona o byte. Não aloca, não envia pela rede e não confere capacidade: destination deve apontar para pelo menos 4 bytes graváveis.

**Parâmetros**

- `uint8_t *destination`: Buffer onde a representação em bytes será escrita.
- `uint32_t value`: Inteiro unsigned a serializar.

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_serialize_header` (protocol.c)](#fn-protocol-c-protocol-serialize-header); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`transfer_fill_transaction_id` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-fill-transaction-id); [`transfer_encode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-chunk); [`encode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-descriptor).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de wire_put_u32, para acompanhar a explicação</summary>

```c
static inline void wire_put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}
```

</details>

<a id="fn-wire-h-wire-get-u32"></a>

#### wire_get_u32

Fonte: `wire.h:24` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline uint32_t wire_get_u32(const uint8_t *source);
```

Reconstrói um inteiro unsigned de 32 bits a partir de 4 bytes big-endian. Cada byte é colocado na posição correspondente e combinado por OR. Não altera source nem valida seu comprimento: o chamador precisa garantir 4 bytes legíveis. Retorna o número, não um status de sucesso.

**Parâmetros**

- `const uint8_t *source`: Buffer de entrada emprestado, contendo os bytes a interpretar.

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_deserialize_header` (protocol.c)](#fn-protocol-c-protocol-deserialize-header); [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`transfer_decode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-chunk); [`decode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-descriptor).

**Retorno:** as expressões presentes nesta função são `((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) | ((uint32_t)source[2] << 8) | source[3]`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de wire_get_u32, para acompanhar a explicação</summary>

```c
static inline uint32_t wire_get_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) | ((uint32_t)source[2] << 8) | source[3];
}
```

</details>

<a id="fn-wire-h-wire-put-u64"></a>

#### wire_put_u64

Fonte: `wire.h:29` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline void wire_put_u64(uint8_t *destination, uint64_t value);
```

Escreve um inteiro unsigned de 64 bits em 8 bytes big-endian. Deslocamentos descartam os bits menos significativos temporariamente, e o cast para uint8_t seleciona o byte. Não aloca, não envia pela rede e não confere capacidade: destination deve apontar para pelo menos 8 bytes graváveis.

**Parâmetros**

- `uint8_t *destination`: Buffer onde a representação em bytes será escrita.
- `uint64_t value`: Inteiro unsigned a serializar.

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_serialize_header` (protocol.c)](#fn-protocol-c-protocol-serialize-header); [`write_manifest` (storage.c)](#fn-storage-c-write-manifest); [`encode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-document-at); [`transfer_fill_transaction_id` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-fill-transaction-id); [`transfer_encode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-chunk); [`transfer_encode_chunk_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-encode-chunk-request); [`encode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-encode-descriptor).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de wire_put_u64, para acompanhar a explicação</summary>

```c
static inline void wire_put_u64(uint8_t *destination, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        destination[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}
```

</details>

<a id="fn-wire-h-wire-get-u64"></a>

#### wire_get_u64

Fonte: `wire.h:39` (linha nesta revisão; pode mudar em futuras edições).

```c
static inline uint64_t wire_get_u64(const uint8_t *source);
```

Reconstrói um inteiro unsigned de 64 bits a partir de 8 bytes big-endian. Cada byte é colocado na posição correspondente e combinado por OR. Não altera source nem valida seu comprimento: o chamador precisa garantir 8 bytes legíveis. Retorna o número, não um status de sucesso.

**Parâmetros**

- `const uint8_t *source`: Buffer de entrada emprestado, contendo os bytes a interpretar.

**Chamadores diretos encontrados nos fontes inventariados:** [`protocol_deserialize_header` (protocol.c)](#fn-protocol-c-protocol-deserialize-header); [`read_manifest` (storage.c)](#fn-storage-c-read-manifest); [`decode_document_at` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-document-at); [`transfer_decode_chunk` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-chunk); [`transfer_decode_chunk_request` (transfer_protocol.c)](#fn-transfer-protocol-c-transfer-decode-chunk-request); [`decode_descriptor` (transfer_protocol.c)](#fn-transfer-protocol-c-decode-descriptor).

**Retorno:** as expressões presentes nesta função são `value`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de wire_get_u64, para acompanhar a explicação</summary>

```c
static inline uint64_t wire_get_u64(const uint8_t *source)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        value = (value << 8) | source[index];
    }
    return value;
}
```

</details>

<a id="mod-tests-c1-test-protocol-c"></a>

### tests/c1/test_protocol.c

[fill_transaction_id](#fn-tests-c1-test-protocol-c-fill-transaction-id) · [test_message_round_trip](#fn-tests-c1-test-protocol-c-test-message-round-trip) · [test_crc32_reference_vector](#fn-tests-c1-test-protocol-c-test-crc32-reference-vector) · [main](#fn-tests-c1-test-protocol-c-main)

<a id="fn-tests-c1-test-protocol-c-fill-transaction-id"></a>

#### fill_transaction_id

Fonte: `tests/c1/test_protocol.c:13` (linha nesta revisão; pode mudar em futuras edições).

```c
static void fill_transaction_id(uint8_t transaction_id[TRANSACTION_ID_SIZE]);
```

Preenche os 16 bytes do TransactionID com um padrão determinístico para comparar envio e recepção no teste. Não é o gerador concorrente de IDs usado em produção.

**Parâmetros**

- `uint8_t transaction_id[TRANSACTION_ID_SIZE]`: 16 bytes de correlação entre requisição e resposta.

**Chamadores diretos encontrados nos fontes inventariados:** [`test_message_round_trip` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-message-round-trip).

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de fill_transaction_id, para acompanhar a explicação</summary>

```c
static void fill_transaction_id(uint8_t transaction_id[TRANSACTION_ID_SIZE])
{
    size_t index;

    for (index = 0U; index < TRANSACTION_ID_SIZE; ++index)
    {
        transaction_id[index] = (uint8_t)(index + 1U);
    }
}
```

</details>

<a id="fn-tests-c1-test-protocol-c-test-message-round-trip"></a>

#### test_message_round_trip

Fonte: `tests/c1/test_protocol.c:24` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_message_round_trip(void);
```

cria socketpair local, envia PING com header/payload e compara a mensagem recebida; verifica liberação dos recursos.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`fill_transaction_id` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-fill-transaction-id).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_message_round_trip, para acompanhar a explicação</summary>

```c
static void test_message_round_trip(void)
{
    static const uint8_t payload[] = {'P', 'I', 'N', 'G'};
    Message sent;
    Message received;
    int sockets[2];
    uint8_t *sent_payload;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(message_init(&sent) == PROTOCOL_OK);
    assert(message_init(&received) == PROTOCOL_OK);

    sent_payload = malloc(sizeof(payload));
    assert(sent_payload != NULL);
    memcpy(sent_payload, payload, sizeof(payload));

    sent.header.message_type = (uint8_t)M_PING;
    memcpy(sent.header.source_node, "source", 6U);
    memcpy(sent.header.destination_node, "dest", 4U);
    fill_transaction_id(sent.header.transaction_id);
    sent.header.timestamp = 123456U;
    sent.header.payload_size = sizeof(payload);
    sent.payload = sent_payload;

    assert(protocol_send_message(sockets[0], &sent) == PROTOCOL_OK);
    assert(protocol_receive_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.message_type == (uint8_t)M_PING);
    assert(received.header.timestamp == sent.header.timestamp);
    assert(received.header.payload_size == sizeof(payload));
    assert(memcmp(received.header.transaction_id, sent.header.transaction_id, TRANSACTION_ID_SIZE) == 0);
    assert(memcmp(received.payload, payload, sizeof(payload)) == 0);

    message_free(&sent);
    message_free(&received);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
}
```

</details>

<a id="fn-tests-c1-test-protocol-c-test-crc32-reference-vector"></a>

#### test_crc32_reference_vector

Fonte: `tests/c1/test_protocol.c:63` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_crc32_reference_vector(void);
```

confere CRC32 do vetor conhecido “123456789”.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`protocol_calculate_crc32` (protocol.c)](#fn-protocol-c-protocol-calculate-crc32).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_crc32_reference_vector, para acompanhar a explicação</summary>

```c
static void test_crc32_reference_vector(void)
{
    static const uint8_t data[] = "123456789";

    assert(protocol_calculate_crc32(data, sizeof(data) - 1U) == UINT32_C(0xcbf43926));
}
```

</details>

<a id="fn-tests-c1-test-protocol-c-main"></a>

#### main

Fonte: `tests/c1/test_protocol.c:71` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(void);
```

Executa os testes de ida/volta de Message e de CRC32 conhecido. Os asserts interrompem a execução em divergência; a mensagem final de sucesso só é alcançada se ambos terminarem.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`test_message_round_trip` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-message-round-trip); [`test_crc32_reference_vector` (tests/c1/test_protocol.c)](#fn-tests-c1-test-protocol-c-test-crc32-reference-vector).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Retorno:** as expressões presentes nesta função são `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(void)
{
    test_message_round_trip();
    test_crc32_reference_vector();
    puts("protocol tests: ok");
    return 0;
}
```

</details>

<a id="mod-tests-c2-test-metadata-c"></a>

### tests/c2/test_metadata.c

[register_peer](#fn-tests-c2-test-metadata-c-register-peer) · [main](#fn-tests-c2-test-metadata-c-main)

<a id="fn-tests-c2-test-metadata-c-register-peer"></a>

#### register_peer

Fonte: `tests/c2/test_metadata.c:14` (linha nesta revisão; pode mudar em futuras edições).

```c
static void *register_peer(void *arg);
```

função de thread que acrescenta um NodeID à disponibilidade do chunk 0.

**Parâmetros**

- `void *arg`: Parâmetro da operação descrita acima; acompanhe suas leituras/atribuições no corpo reproduzido abaixo.

**Funções do projeto usadas diretamente:** [`metadata_register_chunk` (metadata.c)](#fn-metadata-c-metadata-register-chunk).

**Entrada/chamada:** usada como callback ou função de thread/sinal; consulte o registro do callback no módulo.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** as expressões presentes nesta função são `NULL`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de register_peer, para acompanhar a explicação</summary>

```c
static void *register_peer(void *arg)
{
    NodeID peer = {{0}};
    peer.bytes[0] = *(unsigned char *)arg;
    assert(metadata_register_chunk(store, &object, 0, &peer) == 0);
    return NULL;
}
```

</details>

<a id="fn-tests-c2-test-metadata-c-main"></a>

#### main

Fonte: `tests/c2/test_metadata.c:22` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(void);
```

Testa SHA-256 conhecido e incremental, hexadecimal, validação de argumentos, documentos, colisões de buckets, contagem de chunks, idempotência, disponibilidades e registros concorrentes. Cria arquivos temporários próprios e destrói as estruturas no fim; não envia mensagens entre Peers.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`object_id_file` (metadata.c)](#fn-metadata-c-object-id-file); [`object_id_to_hex` (metadata.c)](#fn-metadata-c-object-id-to-hex); [`metadata_create` (metadata.c)](#fn-metadata-c-metadata-create); [`metadata_destroy` (metadata.c)](#fn-metadata-c-metadata-destroy); [`metadata_register_document` (metadata.c)](#fn-metadata-c-metadata-register-document); [`metadata_find_document` (metadata.c)](#fn-metadata-c-metadata-find-document); [`metadata_remove_document` (metadata.c)](#fn-metadata-c-metadata-remove-document); [`metadata_register_chunk` (metadata.c)](#fn-metadata-c-metadata-register-chunk); [`metadata_unregister_chunk` (metadata.c)](#fn-metadata-c-metadata-unregister-chunk); [`metadata_chunk_peers` (metadata.c)](#fn-metadata-c-metadata-chunk-peers); [`metadata_announce` (metadata.c)](#fn-metadata-c-metadata-announce); [`metadata_find_name` (metadata.c)](#fn-metadata-c-metadata-find-name); [`metadata_chunk_descriptor` (metadata.c)](#fn-metadata-c-metadata-chunk-descriptor); [`metadata_remove_peer` (metadata.c)](#fn-metadata-c-metadata-remove-peer).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcmp`: Compara bytes; igualdade é retorno zero. Aqui não se deve confundir igualdade do buffer com autenticação.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `pthread_create`: Inicia thread com callback e contexto; erro vem no retorno pthread, não necessariamente em errno.
- `pthread_join`: Aguarda a thread, garantindo que o contexto dela não seja liberado enquanto ainda estiver em uso.
- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.

**Retorno:** as expressões presentes nesta função são `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(void)
{
    char path[] = "/tmp/aluno2-metadata-XXXXXX";
    int fd = mkstemp(path);
    ObjectID id, empty;
    uint64_t size = 99;
    char hex[65];
    MetadataDocument doc;
    NodeID a = {{1}}, b = {{2}}, *peers = NULL;
    size_t count;
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(object_id_file(path, &empty, &size) == 0 && size == 0);
    assert(object_id_to_hex(&empty, hex, sizeof(hex)) == 0);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fwrite("abc", 1, 3, file) == 3 && fclose(file) == 0);
    assert(object_id_file(path, &id, &size) == 0 && size == 3);
    assert(object_id_to_hex(&id, hex, sizeof(hex)) == 0);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    /* O hash deve atravessar vários buffers de leitura sem carregar o arquivo inteiro. */
    file = fopen(path, "wb");
    assert(file != NULL);
    for (size_t i = 0; i < 200000; ++i) assert(fputc('a', file) == 'a');
    assert(fclose(file) == 0);
    ObjectID large;
    assert(object_id_file(path, &large, &size) == 0 && size == 200000);
    assert(object_id_to_hex(&large, hex, sizeof(hex)) == 0);
    assert(strcmp(hex, "2287d207f24a941ff3b56c04c8a25ad56b63e3023207b3bb5b4ac0c9869d74be") == 0);
    assert(unlink(path) == 0);
    assert(object_id_file(path, &id, &size) == -1 && errno == ENOENT);
    assert(metadata_create(&store) == 0);
    object = id;
    assert(metadata_register_document(store, &id, "exemplo.pdf", METADATA_CHUNK_SIZE + 1) == 0);
    assert(metadata_register_document(store, &id, "exemplo.pdf", METADATA_CHUNK_SIZE + 1) == 0);
    assert(metadata_register_document(store, &id, "outro.pdf", 3) == -1 && errno == EEXIST);
    assert(metadata_find_document(store, &id, &doc) == 0 && doc.chunk_count == 2);
    assert(metadata_register_chunk(store, &id, 2, &a) == -1 && errno == EINVAL);
    assert(metadata_register_chunk(store, &empty, 0, &a) == -1 && errno == ENOENT);
    assert(metadata_register_chunk(store, &id, 0, &a) == 0);
    assert(metadata_register_chunk(store, &id, 0, &a) == 0);
    assert(metadata_register_chunk(store, &id, 0, &b) == 0);
    assert(metadata_chunk_peers(store, &id, 0, &peers, &count) == 0 && count == 2);
    free(peers);
    assert(metadata_chunk_peers(store, &id, 1, &peers, &count) == 0 && count == 0 && peers == NULL);
    assert(metadata_unregister_chunk(store, &id, 0, &a) == 0);
    assert(metadata_unregister_chunk(store, &id, 0, &a) == -1 && errno == ENOENT);
    pthread_t threads[8];
    unsigned char ids[8];
    for (size_t i = 0; i < 8; ++i) { ids[i] = (unsigned char)(i + 3); assert(pthread_create(&threads[i], NULL, register_peer, &ids[i]) == 0); }
    for (size_t i = 0; i < 8; ++i) { assert(pthread_join(threads[i], NULL) == 0); }
    assert(metadata_chunk_peers(store, &id, 0, &peers, &count) == 0 && count == 9);
    free(peers);
    /* Mais documentos que buckets garantem colisões sem perda de registros. */
    for (unsigned int i = 0; i < 600; ++i) { ObjectID key = {{0}}; key.bytes[0] = (unsigned char)i; key.bytes[1] = (unsigned char)(i >> 8); assert(metadata_register_document(store, &key, "colisao.pdf", 0) == 0); }
    for (unsigned int i = 0; i < 600; ++i) { ObjectID key = {{0}}; key.bytes[0] = (unsigned char)i; key.bytes[1] = (unsigned char)(i >> 8); assert(metadata_find_document(store, &key, &doc) == 0 && doc.chunk_count == 0); }
    assert(metadata_remove_document(store, &id) == 0);
    assert(metadata_find_document(store, &id, &doc) == -1 && errno == ENOENT);
    assert(metadata_register_document(store, &empty, "", 0) == -1 && errno == EINVAL);
    /* Fronteiras de tamanho, aliases e saídas independentes da tabela. */
    assert(metadata_register_document(store, &empty, "vazio.pdf", 0) == 0);
    assert(metadata_register_chunk(store, &empty, 0, &a) == -1 && errno == EINVAL);
    assert(metadata_register_document(store, &id, "inteiro.pdf", METADATA_CHUNK_SIZE) == 0);
    assert(metadata_register_document(store, &id, "alias.pdf", METADATA_CHUNK_SIZE) == 0);
    assert(metadata_find_document(store, &id, &doc) == 0 && doc.chunk_count == 1 && strcmp(doc.name, "inteiro.pdf") == 0);
    assert(metadata_register_chunk(store, &id, 0, &a) == 0);
    assert(metadata_chunk_peers(store, &id, 0, &peers, &count) == 0 && count == 1);
    assert(metadata_remove_document(store, &id) == 0);
    assert(memcmp(peers[0].bytes, a.bytes, NODE_ID_SIZE) == 0);
    free(peers);
    assert(metadata_register_document(store, &id, "maximo.pdf", UINT64_MAX) == 0);
    assert(metadata_find_document(store, &id, &doc) == 0 && doc.chunk_count == UINT64_MAX / METADATA_CHUNK_SIZE + 1);
    assert(metadata_register_chunk(store, &id, doc.chunk_count - 1, &a) == 0);
    char long_name[METADATA_NAME_SIZE + 1];
    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    assert(metadata_register_document(store, &id, long_name, 0) == -1 && errno == EINVAL);
    assert(metadata_create(NULL) == -1 && errno == EINVAL);
    assert(metadata_find_document(NULL, &id, &doc) == -1 && errno == EINVAL);
    assert(object_id_to_hex(&id, hex, 64) == -1 && errno == EINVAL);
    /* Em erro, a API não sobrescreve os dados que pertencem ao chamador. */
    MetadataDocument saved = doc;
    assert(metadata_find_document(store, &large, &doc) == -1 && errno == ENOENT);
    assert(memcmp(&saved, &doc, sizeof(doc)) == 0);
    peers = &a;
    count = 42;
    assert(metadata_chunk_peers(store, &large, 0, &peers, &count) == -1 && errno == ENOENT);
    assert(peers == &a && count == 42);
    ObjectID previous = id;
    size = 42;
    assert(object_id_file(path, &id, &size) == -1 && errno == ENOENT);
    assert(memcmp(previous.bytes, id.bytes, OBJECT_ID_SIZE) == 0 && size == 42);
    /* Anúncio completo deve publicar todos os chunks ou preservar o registro anterior. */
    MetadataDocument announced = {.id = large, .name = "completo.pdf", .file_size = 3U, .chunk_count = 1U, .compression = 1U};
    MetadataChunk descriptor = {.index = 0U, .offset = 0U, .raw_size = 3U, .compressed_size = 4U, .hash = {7U}};
    assert(metadata_announce(store, &announced, &descriptor, &a) == 0);
    assert(metadata_announce(store, &announced, &descriptor, &b) == 0);
    assert(metadata_find_document(store, &large, &doc) == 0 && doc.version == 1U && doc.compression == 1U && memcmp(doc.owner.bytes, a.bytes, NODE_ID_SIZE) == 0);
    assert(metadata_chunk_peers(store, &large, 0U, &peers, &count) == 0 && count == 2U);
    free(peers);
    descriptor.hash[0] = 8U;
    assert(metadata_announce(store, &announced, &descriptor, &a) == -1 && errno == EEXIST);
    assert(metadata_chunk_descriptor(store, &large, 0U, &descriptor) == 0 && descriptor.hash[0] == 7U);
    assert(metadata_remove_peer(store, &a) == 0);
    assert(metadata_chunk_peers(store, &large, 0U, &peers, &count) == 0 && count == 1U);
    free(peers);
    assert(metadata_find_name(store, "completo.pdf", &previous) == 0 && memcmp(previous.bytes, large.bytes, OBJECT_ID_SIZE) == 0);
    metadata_destroy(store);
    puts("metadata C2: ok");
    return 0;
}
```

</details>

<a id="mod-tests-c2-test-storage-atomic-c"></a>

### tests/c2/test_storage_atomic.c

[main](#fn-tests-c2-test-storage-atomic-c-main)

<a id="fn-tests-c2-test-storage-atomic-c-main"></a>

#### main

Fonte: `tests/c2/test_storage_atomic.c:13` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(void);
```

Constrói um documento pequeno e um chunk válido. Cria um obstáculo no caminho temporário do manifest para provocar falha real de persistência; exige erro, retira o obstáculo e repete a gravação, verificando que o estado não foi falsamente confirmado. Também verifica duplicata incompatível. Limpa apenas os artefatos de teste criados.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`object_id_to_hex` (metadata.c)](#fn-metadata-c-object-id-to-hex); [`storage_create` (storage.c)](#fn-storage-c-storage-create); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit); [`storage_find` (storage.c)](#fn-storage-c-storage-find).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** as expressões presentes nesta função são `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(void)
{
    char root[] = "/tmp/c2-atomic-XXXXXX";
    char object[OBJECT_ID_HEX_SIZE], obstacle[512];
    NodeID owner = {{1}};
    Storage *storage = NULL;
    TransferDocument document = {.name = "atomic.pdf", .file_size = 3U, .chunk_count = 1U, .compression = COMPRESSION_LZ4};
    TransferDocument committed;
    uint8_t *compressed = NULL;
    size_t size;
    assert(mkdtemp(root) != NULL);
    assert(content_sha256((const uint8_t *)"abc", 3U, document.id.bytes) == 0);
    assert(object_id_to_hex(&document.id, object, sizeof(object)) == 0);
    assert(compression_lz4_compress((const uint8_t *)"abc", 3U, &compressed, &size) == 0);
    TransferChunk chunk = {.id = document.id, .index = 0U, .offset = 0U, .raw_size = 3U, .compressed_size = (uint32_t)size, .data = compressed};
    memcpy(chunk.hash, document.id.bytes, OBJECT_ID_SIZE);
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    (void)snprintf(obstacle, sizeof(obstacle), "%s/pending/%s/manifest.tmp-%ld", root, object, (long)getpid());
    assert(mkdir(obstacle, 0700) == 0);
    assert(storage_put_chunk(storage, &chunk) == -1);
    assert(storage_commit(storage, &document.id, &committed) == -1 && errno == ENODATA);
    assert(rmdir(obstacle) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    assert(mkdir(obstacle, 0700) == 0);
    assert(storage_commit(storage, &document.id, &committed) == -1);
    assert(rmdir(obstacle) == 0);
    assert(storage_commit(storage, &document.id, &committed) == 0);
    free(compressed);
    assert(compression_lz4_compress((const uint8_t *)"xyz", 3U, &compressed, &size) == 0);
    chunk.data = compressed;
    chunk.compressed_size = (uint32_t)size;
    assert(content_sha256((const uint8_t *)"xyz", 3U, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == -1 && errno == EEXIST);
    free(compressed);
    storage_destroy(storage);
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_find(storage, TRANSFER_SELECTOR_OBJECT_ID, &document.id, NULL, &committed) == 0);
    storage_destroy(storage);
    printf("storage atomic/fault tests: ok; evidências: %s\n", root);
    return 0;
}
```

</details>

<a id="mod-tests-c2-test-transfer-negative-c"></a>

### tests/c2/test_transfer_negative.c

[make_wire_message](#fn-tests-c2-test-transfer-negative-c-make-wire-message) · [test_protocol_rejections](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections) · [cleanup_storage](#fn-tests-c2-test-transfer-negative-c-cleanup-storage) · [test_storage_validation](#fn-tests-c2-test-transfer-negative-c-test-storage-validation) · [test_object_id_mismatch](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch) · [test_pending_restart](#fn-tests-c2-test-transfer-negative-c-test-pending-restart) · [main](#fn-tests-c2-test-transfer-negative-c-main)

<a id="fn-tests-c2-test-transfer-negative-c-make-wire-message"></a>

#### make_wire_message

Fonte: `tests/c2/test_transfer_negative.c:17` (linha nesta revisão; pode mudar em futuras edições).

```c
static void make_wire_message(uint8_t output[HEADER_WIRE_SIZE + 4U]);
```

produz uma mensagem PING válida em bytes para que outros testes a adulterem.

**Parâmetros**

- `uint8_t output[HEADER_WIRE_SIZE + 4U]`: Objeto/buffer de saída fornecido pelo chamador; o tamanho e a validade exigidos aparecem nas checagens abaixo.

**Funções do projeto usadas diretamente:** [`network_recv_exact` (network.c)](#fn-network-c-network-recv-exact); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`protocol_send_message` (protocol.c)](#fn-protocol-c-protocol-send-message).

**Chamadores diretos encontrados nos fontes inventariados:** [`test_protocol_rejections` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de make_wire_message, para acompanhar a explicação</summary>

```c
static void make_wire_message(uint8_t output[HEADER_WIRE_SIZE + 4U])
{
    int pair[2];
    Message message;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(message_init(&message) == 0);
    message.header.message_type = M_PING;
    message.header.payload_size = 4U;
    message.payload = (uint8_t *)"PING";
    assert(protocol_send_message(pair[0], &message) == PROTOCOL_OK);
    assert(network_recv_exact(pair[1], output, HEADER_WIRE_SIZE + 4U) == (ssize_t)(HEADER_WIRE_SIZE + 4U));
    message.payload = NULL;
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);
}
```

</details>

<a id="fn-tests-c2-test-transfer-negative-c-test-protocol-rejections"></a>

#### test_protocol_rejections

Fonte: `tests/c2/test_transfer_negative.c:34` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_protocol_rejections(void);
```

modifica CRC, trunca payload e aumenta tamanho anunciado além do limite; espera PROTOCOL_ERROR.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`network_send_all` (network.c)](#fn-network-c-network-send-all); [`message_init` (protocol.c)](#fn-protocol-c-message-init); [`message_free` (protocol.c)](#fn-protocol-c-message-free); [`protocol_receive_message` (protocol.c)](#fn-protocol-c-protocol-receive-message); [`make_wire_message` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-make-wire-message).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `close`: Libera um descritor do processo. Fechar não equivale a apagar o arquivo e não substitui fsync.
- `shutdown`: Interrompe direções de comunicação do socket; o descritor ainda precisa de close.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_protocol_rejections, para acompanhar a explicação</summary>

```c
static void test_protocol_rejections(void)
{
    uint8_t wire[HEADER_WIRE_SIZE + 4U];
    int pair[2];
    Message received;

    make_wire_message(wire);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    wire[HEADER_WIRE_SIZE] ^= 1U;
    assert(network_send_all(pair[0], wire, sizeof(wire)) == (ssize_t)sizeof(wire));
    assert(message_init(&received) == 0);
    assert(protocol_receive_message(pair[1], &received) == PROTOCOL_ERROR);
    message_free(&received);
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);

    make_wire_message(wire);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(network_send_all(pair[0], wire, HEADER_WIRE_SIZE + 2U) == (ssize_t)(HEADER_WIRE_SIZE + 2U));
    assert(shutdown(pair[0], SHUT_WR) == 0);
    assert(message_init(&received) == 0);
    assert(protocol_receive_message(pair[1], &received) == PROTOCOL_ERROR);
    message_free(&received);
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);

    make_wire_message(wire);
    wire[90] = 0U;
    wire[91] = 0x50U;
    wire[92] = 0U;
    wire[93] = 1U;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(network_send_all(pair[0], wire, HEADER_WIRE_SIZE) == (ssize_t)HEADER_WIRE_SIZE);
    assert(message_init(&received) == 0);
    assert(protocol_receive_message(pair[1], &received) == PROTOCOL_ERROR);
    message_free(&received);
    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);
}
```

</details>

<a id="fn-tests-c2-test-transfer-negative-c-cleanup-storage"></a>

#### cleanup_storage

Fonte: `tests/c2/test_transfer_negative.c:74` (linha nesta revisão; pode mudar em futuras edições).

```c
static void cleanup_storage(const char *root, const ObjectID *id);
```

remove apenas os arquivos/diretórios temporários específicos criados por estes testes.

**Parâmetros**

- `const char *root`: Diretório raiz do armazenamento persistente.
- `const ObjectID *id`: ObjectID/NodeID conforme o tipo; não é um caminho nem um inteiro de processo.

**Funções do projeto usadas diretamente:** [`object_id_to_hex` (metadata.c)](#fn-metadata-c-object-id-to-hex).

**Chamadores diretos encontrados nos fontes inventariados:** [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `snprintf`: Monta texto com capacidade limitada. Retorno maior ou igual à capacidade indica truncamento e deve ser rejeitado.
- `unlink`: Remove um nome de arquivo/socket. Não é exclusão recursiva e não deve atingir arquivo que a operação não criou.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de cleanup_storage, para acompanhar a explicação</summary>

```c
static void cleanup_storage(const char *root, const ObjectID *id)
{
    char hex[OBJECT_ID_HEX_SIZE];
    char path[512];

    assert(object_id_to_hex(id, hex, sizeof(hex)) == 0);
    assert(snprintf(path, sizeof(path), "%s/pending/%s/manifest.bin", root, hex) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/pending/%s/chunk-%020u.lz4", root, hex, 0U) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/pending/%s/chunk-%020u.lz4", root, hex, 1U) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/pending/%s", root, hex) > 0);
    (void)rmdir(path);
    assert(snprintf(path, sizeof(path), "%s/pending", root) > 0);
    (void)rmdir(path);
    assert(snprintf(path, sizeof(path), "%s/objects", root) > 0);
    (void)rmdir(path);
    (void)rmdir(root);
}
```

</details>

<a id="fn-tests-c2-test-transfer-negative-c-test-storage-validation"></a>

#### test_storage_validation

Fonte: `tests/c2/test_transfer_negative.c:95` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_storage_validation(void);
```

verifica que SHA de chunk inválido, índice/offset incorretos e duplicata incompatível são recusados.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`storage_create` (storage.c)](#fn-storage-c-storage-create); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`cleanup_storage` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-cleanup-storage).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `malloc`: Reserva memória sem zerar. O teste de NULL separa falha de alocação; a propriedade do resultado depende de onde ele é guardado ou devolvido.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memcpy`: Copia a quantidade explícita de bytes, inclusive zeros. Não acrescenta terminador de string nem converte endianness.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_storage_validation, para acompanhar a explicação</summary>

```c
static void test_storage_validation(void)
{
    static const uint8_t raw[] = "%PDF-1.4\n%%EOF\n";
    char root_template[] = "/tmp/p2p-storage-test-XXXXXX";
    char *root = mkdtemp(root_template);
    NodeID owner;
    Storage *storage = NULL;
    TransferDocument document;
    TransferChunk chunk;
    uint8_t *compressed = NULL;
    uint8_t *incompatible = NULL;
    size_t compressed_size = 0U;

    assert(root != NULL);
    memset(&owner, 0x5a, sizeof(owner));
    memset(&document, 0, sizeof(document));
    assert(content_sha256(raw, sizeof(raw) - 1U, document.id.bytes) == 0);
    strcpy(document.name, "negative.pdf");
    document.file_size = sizeof(raw) - 1U;
    document.chunk_count = 1U;
    document.compression = COMPRESSION_LZ4;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(compression_lz4_compress(raw, sizeof(raw) - 1U, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document.id;
    chunk.raw_size = (uint32_t)(sizeof(raw) - 1U);
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    memset(chunk.hash, 0, sizeof(chunk.hash));
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0 && errno == EBADMSG);
    assert(content_sha256(raw, sizeof(raw) - 1U, chunk.hash) == 0);
    chunk.index = 1U;
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0 && errno == EINVAL);
    chunk.index = 0U;
    chunk.offset = 1U;
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0 && errno == EINVAL);
    chunk.offset = 0U;
    assert(storage_put_chunk(storage, &chunk) == 0);
    incompatible = malloc(compressed_size + 1U);
    assert(incompatible != NULL);
    memcpy(incompatible, compressed, compressed_size);
    incompatible[compressed_size] = 0U;
    chunk.data = incompatible;
    chunk.compressed_size++;
    errno = 0;
    assert(storage_put_chunk(storage, &chunk) < 0);
    free(incompatible);
    free(compressed);
    storage_destroy(storage);
    cleanup_storage(root, &document.id);
}
```

</details>

<a id="fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch"></a>

#### test_object_id_mismatch

Fonte: `tests/c2/test_transfer_negative.c:151` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_object_id_mismatch(void);
```

usa chunks individualmente válidos, mas ObjectID final adulterado; COMMIT precisa falhar.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`storage_create` (storage.c)](#fn-storage-c-storage-create); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`storage_commit` (storage.c)](#fn-storage-c-storage-commit); [`cleanup_storage` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-cleanup-storage).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_object_id_mismatch, para acompanhar a explicação</summary>

```c
static void test_object_id_mismatch(void)
{
    static const uint8_t raw[] = "%PDF-1.4\nobjeto divergente\n%%EOF\n";
    char root_template[] = "/tmp/p2p-object-test-XXXXXX";
    char *root = mkdtemp(root_template);
    NodeID owner;
    Storage *storage = NULL;
    TransferDocument document;
    TransferDocument committed;
    TransferChunk chunk;
    uint8_t *compressed = NULL;
    size_t compressed_size = 0U;

    assert(root != NULL);
    memset(&owner, 0x3c, sizeof(owner));
    memset(&document, 0, sizeof(document));
    assert(content_sha256(raw, sizeof(raw) - 1U, document.id.bytes) == 0);
    document.id.bytes[0] ^= 1U;
    strcpy(document.name, "mismatch.pdf");
    document.file_size = sizeof(raw) - 1U;
    document.chunk_count = 1U;
    document.compression = COMPRESSION_LZ4;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(compression_lz4_compress(raw, sizeof(raw) - 1U, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document.id;
    chunk.raw_size = (uint32_t)(sizeof(raw) - 1U);
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    assert(content_sha256(raw, sizeof(raw) - 1U, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    errno = 0;
    assert(storage_commit(storage, &document.id, &committed) < 0 && errno == EBADMSG);
    free(compressed);
    storage_destroy(storage);
    cleanup_storage(root, &document.id);
}
```

</details>

<a id="fn-tests-c2-test-transfer-negative-c-test-pending-restart"></a>

#### test_pending_restart

Fonte: `tests/c2/test_transfer_negative.c:190` (linha nesta revisão; pode mudar em futuras edições).

```c
static void test_pending_restart(void);
```

deixa upload incompleto, destrói/recria Storage, retoma o primeiro chunk e grava o seguinte; verifica recuperação do manifest pending.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`compression_lz4_compress` (compression.c)](#fn-compression-c-compression-lz4-compress); [`content_sha256` (content.c)](#fn-content-c-content-sha256); [`storage_create` (storage.c)](#fn-storage-c-storage-create); [`storage_destroy` (storage.c)](#fn-storage-c-storage-destroy); [`storage_begin` (storage.c)](#fn-storage-c-storage-begin); [`storage_put_chunk` (storage.c)](#fn-storage-c-storage-put-chunk); [`cleanup_storage` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-cleanup-storage).

**Chamadores diretos encontrados nos fontes inventariados:** [`main` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-main).

**Como ler as operações relevantes do corpo** (nem todos os caminhos executam todas):

- `calloc`: Reserva memória inicialmente zerada. Tamanhos derivados da rede precisam ser limitados antes da alocação.
- `free`: Libera uma alocação, não o recurso apontado por campos internos automaticamente. free(NULL) é permitido.
- `memset`: Preenche bytes, frequentemente para inicializar estruturas. Zerar um ponteiro de payload antigo não libera sua alocação.
- `assert`: Expressa uma propriedade do teste. Se falsa, aborta; só faz parte das verificações se asserts não forem desabilitados.

**Retorno:** não entrega um valor útil ao chamador; os efeitos ocorrem nas saídas, no estado ou nos recursos manipulados.

<details>
<summary>Código completo de test_pending_restart, para acompanhar a explicação</summary>

```c
static void test_pending_restart(void)
{
    static const uint8_t tail[] = "x";
    char root_template[] = "/tmp/p2p-resume-test-XXXXXX";
    char *root = mkdtemp(root_template);
    NodeID owner;
    Storage *storage = NULL;
    TransferDocument document;
    TransferChunk chunk;
    uint8_t *raw = NULL;
    uint8_t *compressed = NULL;
    size_t compressed_size = 0U;

    assert(root != NULL);
    raw = calloc((size_t)METADATA_CHUNK_SIZE, 1U);
    assert(raw != NULL);
    memset(&owner, 0x17, sizeof(owner));
    memset(&document, 0, sizeof(document));
    document.id.bytes[0] = 0x42U;
    strcpy(document.name, "resume.pdf");
    document.file_size = METADATA_CHUNK_SIZE + 1U;
    document.chunk_count = 2U;
    document.compression = COMPRESSION_LZ4;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(compression_lz4_compress(raw, (size_t)METADATA_CHUNK_SIZE, &compressed, &compressed_size) == 0);
    memset(&chunk, 0, sizeof(chunk));
    chunk.id = document.id;
    chunk.raw_size = (uint32_t)METADATA_CHUNK_SIZE;
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    assert(content_sha256(raw, (size_t)METADATA_CHUNK_SIZE, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    storage_destroy(storage);
    storage = NULL;
    assert(storage_create(root, &owner, &storage) == 0);
    assert(storage_begin(storage, &document) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    free(compressed);
    compressed = NULL;
    assert(compression_lz4_compress(tail, sizeof(tail) - 1U, &compressed, &compressed_size) == 0);
    chunk.index = 1U;
    chunk.offset = METADATA_CHUNK_SIZE;
    chunk.raw_size = (uint32_t)(sizeof(tail) - 1U);
    chunk.compressed_size = (uint32_t)compressed_size;
    chunk.data = compressed;
    assert(content_sha256(tail, sizeof(tail) - 1U, chunk.hash) == 0);
    assert(storage_put_chunk(storage, &chunk) == 0);
    free(compressed);
    free(raw);
    storage_destroy(storage);
    cleanup_storage(root, &document.id);
}
```

</details>

<a id="fn-tests-c2-test-transfer-negative-c-main"></a>

#### main

Fonte: `tests/c2/test_transfer_negative.c:244` (linha nesta revisão; pode mudar em futuras edições).

```c
int main(void);
```

Executa rejeições de protocolo, validação de chunks, ObjectID final divergente e retomada de pending. Os asserts são os critérios de aprovação; o processo retorna sucesso somente depois dos quatro cenários.

Não recebe parâmetros explícitos. Variáveis globais, quando acessadas, continuam sendo dependências da função.

**Funções do projeto usadas diretamente:** [`test_protocol_rejections` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-protocol-rejections); [`test_storage_validation` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-storage-validation); [`test_object_id_mismatch` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-object-id-mismatch); [`test_pending_restart` (tests/c2/test_transfer_negative.c)](#fn-tests-c2-test-transfer-negative-c-test-pending-restart).

**Entrada/chamada:** iniciada pelo runtime do executável.

**Retorno:** as expressões presentes nesta função são `0`. O significado depende do caminho: os detalhes acima e as condições no corpo distinguem sucesso, ausência e falha.

<details>
<summary>Código completo de main, para acompanhar a explicação</summary>

```c
int main(void)
{
    test_protocol_rejections();
    test_storage_validation();
    test_object_id_mismatch();
    test_pending_restart();
    puts("transfer/storage negative tests: ok");
    return 0;
}
```

</details>

## 27. Python e Bash — funções e execução dos scripts

Os argumentos Bash são $1, $2 etc.; o retorno do shell é um status numérico (zero indica sucesso), enquanto os resultados também podem ser comunicados por variáveis globais e arquivos. Em Python, uma exceção interrompe o cenário e o bloco finally continua responsável pela limpeza. As seções abaixo incluem os corpos principais dos scripts porque grande parte dos testes não está dentro de funções.

<a id="script-tests-c2-integration-py"></a>

### tests/c2/integration.py

#### check

Recebe condição e descrição. Se a condição for falsa, lança AssertionError, interrompendo o cenário; se verdadeira, incrementa o contador global e imprime PASS. Não retorna um relatório estruturado.

#### port

Abre um socket temporário, faz bind em 127.0.0.1:0 e consulta a porta escolhida pelo kernel. Fecha o socket e devolve o número. Isso escolhe uma porta aparentemente livre, mas não a reserva até o servidor iniciar.

#### launch

Recebe nome, argumentos do processo e marcador de prontidão. Cria log exclusivo no diretório temporário, inicia subprocesso e espera o marcador com limite de tempo, conferindo se o processo morreu. Registra processos e logs para cleanup; em falha inclui evidência do log na exceção.

#### start_sp

Wrapper de launch para iniciar o Super Peer de teste na porta selecionada, esperando sua mensagem de inicialização. Devolve o objeto de processo para encerramento posterior.

#### start_peer

Wrapper de launch que inicia um Peer de armazenamento, passando a porta local e o endpoint do Super Peer. Espera o marcador do serviço; cada processo tem log próprio.

#### stop

Recebe um processo e a opção crash. Se ainda estiver vivo, usa SIGTERM normalmente ou SIGKILL para simular queda; depois aguarda a saída com limite. Encerrar deliberadamente um processo de teste é diferente de detectar falhas no sistema distribuído.

#### command

Executa a CLI com --local-peer-port para selecionar o executor ativo. Captura stdout/stderr, limita a duração, acrescenta evidência ao log e confere se o código de saída coincide com o sucesso ou falha esperados. Devolve o texto combinado para outras verificações.

#### node_id

Lê o log identificado pelo nome e usa expressão regular para extrair os 64 dígitos do NodeID. É uma leitura de evidência de teste, não a geração de identidade de produção.

#### exact

Repete recv até reunir exatamente a quantidade solicitada. Se receber EOF antes disso, lança EOFError. Retorna bytes completos; auxilia a implementação independente do protocolo usada nos testes.

#### frame

Constrói um frame sem chamar o código C: empacota header big-endian, acrescenta payload e calcula CRC32 com o campo CRC inicialmente zerado. Permite escolher tipo, origem, destino, versão e TransactionID, inclusive valores inválidos para testes negativos.

#### reply

Lê exatamente 98 bytes, desempacota os campos, recebe o corpo pelo comprimento anunciado e verifica CRC32. Retorna campos e payload. Um assert de CRC falho indica que a resposta não cumpre o contrato do teste.

#### exchange

Função aninhada no cenário: abre conexão com Peer A, envia STORE com identidade de B e destino A e lê resposta. Usa as variáveis do cenário externo; não recebe todos esses dados como argumentos.

#### begin

Produz bytes de STORE/BEGIN para um documento curto, com nome e ObjectID configuráveis. O retorno é payload, não resultado de upload. Serve para provocar validações específicas do receptor.

#### chunk

Produz bytes de STORE/CHUNK para o bloco de teste, permitindo adulterar ObjectID, índice, offset ou hash. O conteúdo comprimido já foi preparado no cenário; a função não é um worker real.

#### stall

Callback da thread que simula endpoint defeituoso: aceita TCP e mantém a conexão sem responder até sinalização/limite. Permite provar timeout e fallback, não apenas falha imediata de connect.

O corpo principal monta cenários de tamanhos 62.000 bytes, 4 MiB−1, 4 MiB, 4 MiB+1 e 10 MiB; faz upload em A/download em B, compara bytes, observa concorrência, verifica IDs, injeta frames malformados, testa persistência, erro de bind, fallback e endpoint silencioso. Usa diretório temporário e guarda evidências. A contagem de checks depende das chamadas efetivamente executadas; não é um certificado universal de correção.

<details>
<summary>Código completo de tests/c2/integration.py</summary>

```python
#!/usr/bin/env python3
"""Testes independentes C1/C2; logs e artefatos ficam em diretório temporário informado."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time
import zlib
import threading
import ctypes

parser = argparse.ArgumentParser()
parser.add_argument("--bin-dir", default="bin")
args = parser.parse_args()
bins = Path(args.bin_dir).resolve()
root = Path(tempfile.mkdtemp(prefix="pd-c2-integration-"))
processes = []
logs = {}
env = dict(os.environ, PEER_IO_TIMEOUT="2", PEER_CONNECT_TIMEOUT="1", PEER_TRANSFER_THREADS="4")
passed = 0

def check(condition, description):
    global passed
    if not condition:
        raise AssertionError(description)
    passed += 1
    print(f"PASS {description}", flush=True)

def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

sp_port, a_port, b_port = port(), port(), port()

def launch(name, arguments, marker):
    log = root / (name + ".log")
    handle = log.open("w")
    process = subprocess.Popen([str(bins / arguments[0]), *map(str, arguments[1:])], cwd=root, stdout=handle, stderr=subprocess.STDOUT, env=env)
    handle.close()
    processes.append(process)
    logs[name] = log
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"{name} encerrou: {log.read_text()}")
        if marker in log.read_text():
            return process
        time.sleep(.05)
    raise AssertionError(f"{name} não iniciou: {log.read_text()}")

def start_sp(name="sp"):
    return launch(name, ["superpeer", "--port", sp_port, "--name", "integration"], "NodeID:")

def start_peer(name, number):
    return launch(name, ["peer", "serve", number, "127.0.0.1", sp_port], "Peer storage started")

def stop(process, crash=False):
    if process.poll() is None:
        process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        process.wait(timeout=15)

def command(local, *arguments, ok=True):
    result = subprocess.run([str(bins / "peer"), "--local-peer-port", str(local), *map(str, arguments)], cwd=root, text=True, capture_output=True, env=env, timeout=60)
    with (root / "commands.log").open("a") as log:
        log.write(repr(arguments) + "\n" + result.stdout + result.stderr)
    if ok and result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    if not ok and result.returncode == 0:
        raise AssertionError("Comando inválido foi aceito")
    return result.stdout + result.stderr

def node_id(name):
    return re.search(r"NodeID: ([0-9a-f]{64})", logs[name].read_text()).group(1)

def exact(s, size):
    data = b""
    while len(data) < size:
        part = s.recv(size - len(data))
        if not part:
            raise EOFError("Frame truncado")
        data += part
    return data

def frame(kind, payload=b"PING", source=bytes(32), dest=bytes(32), version=1, tx=None):
    tx = tx or os.urandom(16)
    header = struct.pack("!BB32s32s16sQII", version, kind, source, dest, tx, int(time.time()), len(payload), 0)
    crc = zlib.crc32(header + payload)
    return header[:-4] + struct.pack("!I", crc) + payload

def reply(s):
    header = exact(s, 98)
    fields = struct.unpack("!BB32s32s16sQII", header)
    payload = exact(s, fields[6])
    assert zlib.crc32(header[:-4] + bytes(4) + payload) == fields[7]
    return fields, payload

try:
    sp = start_sp()
    a = start_peer("a", a_port)
    b = start_peer("b", b_port)
    aid, bid = node_id("a"), node_id("b")
    check(aid != bid, "identidades distintas")
    samples = []
    for i, size in enumerate([62000, 4194303, 4194304, 4194305, 10485760]):
        path = root / f"sample-{i}.PDF"
        # Conteúdo sintético sem assinatura, como as fixtures do professor.
        path.write_bytes(os.urandom(size) if i == 4 else (bytes(range(256)) * ((size + 255) // 256))[:size])
        samples.append(path)
        output = command(a_port, "upload", path, "127.0.0.1", a_port)
        if i == 4:
            active = peak = 0
            for line in output.splitlines():
                if line.startswith("Worker start"): active += 1
                if line.startswith("Worker finish"): active -= 1
                peak = max(peak, active)
            check(peak > 1 and active == 0, "workers realmente sobrepostos durante upload")
        expected_id = hashlib.sha256(path.read_bytes()).hexdigest()
        check(f"ObjectID: {expected_id}" in output and f"Chunks: {(size + 4194303) // 4194304}\n" in output, f"upload e chunking {size} bytes")
        target = root / f"download-{i}.pdf"
        output = command(b_port, "download", expected_id if i % 2 else path.name, target, "127.0.0.1", sp_port)
        check(target.read_bytes() == path.read_bytes() and "SHA-256 verified" in output, f"download Peer B {size} bytes")
    duplicate = subprocess.run([str(bins / "peer"), "serve", str(a_port), "127.0.0.1", str(sp_port)], cwd=root, capture_output=True, text=True, env=env, timeout=10)
    check(duplicate.returncode != 0, "falha de bind rejeita segunda instância")
    command(b_port, "download", samples[0].name, root / "after-bind-failure.pdf", "127.0.0.1", sp_port)
    check((root / "after-bind-failure.pdf").exists(), "falha de bind não remove cadastro do Peer ativo")
    before_fds = len(list(Path(f"/proc/{b.pid}/fd").iterdir()))
    for i in range(10):
        command(b_port, "download", samples[0].name, root / f"fd-{i}.pdf", "127.0.0.1", sp_port)
    time.sleep(.1)
    check(len(list(Path(f"/proc/{b.pid}/fd").iterdir())) <= before_fds + 1, "descritores estáveis após dez operações")
    config_port = port()
    conf = root / "peer.conf"
    conf.write_text(f"ip=127.0.0.2\nport=1\ndata-dir={root / 'configured-storage'}\nsuperpeer-host=127.0.0.1\nsuperpeer-port={sp_port}\n")
    configured = launch("configured", ["peer", "serve", "--config", conf, "--bind", "127.0.0.2", "--port", config_port], "Peer storage started")
    configured_file = root / "configured.pdf"
    configured_file.write_bytes(b"config-file-content")
    command(b_port, "upload", configured_file, "127.0.0.2", config_port)
    result = command(b_port, "download", configured_file.name, root / "configured-download.pdf", "127.0.0.1", sp_port)
    check(f"127.0.0.2:{config_port}" in result and (root / "configured-storage" / "node.uuid").exists(), "configuração lida, IP anunciado e precedência CLI")
    stop(configured)
    check(f"tipo=6, origem={bid}" in logs["sp"].read_text(), "LOOKUP usa NodeID do Peer B")
    check(f"tipo=8 origem={bid}" in logs["a"].read_text(), "DOWNLOAD_REQ usa NodeID do Peer B")
    check("origem=" + "0" * 64 not in logs["sp"].read_text(), "operações C2 sem origem anônima")

    for count in [1, 2]:
        with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
            request = frame(3)
            if count == 1:
                for index in range(0, len(request), 7): s.sendall(request[index:index+7])
            else:
                s.sendall(request * 2)
            for _ in range(count):
                fields, payload = reply(s)
                check(fields[1] == 4 and payload == b"PONG" and fields[4] == request[66:82], "framing independente e TransactionID preservado")
    oversized = struct.pack("!BB32s32s16sQII", 1, 3, bytes(32), bytes(32), os.urandom(16), 0, 5 * 1024 * 1024 + 1, 0)
    for invalid in [frame(3, version=99), frame(3)[:-1] + b"X", frame(3)[:40], oversized]:
        with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
            s.sendall(invalid)
            s.shutdown(socket.SHUT_WR)
            try: closed = s.recv(1) == b""
            except ConnectionResetError: closed = True
            check(closed, "frame inválido rejeitado por fechamento, não timeout")
    with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
        s.sendall(frame(6, b"bad"))
        fields, data = reply(s)
        check(fields[1] == 2 and data == bytes([1, 6]), "LOOKUP sem identidade rejeitado especificamente")
    with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
        s.sendall(frame(13, b""))
        fields, data = reply(s)
        check(fields[1] == 2 and data == bytes([1, 5]), "heartbeat permanece não implementado")

    # Serialização independente e corrupção em requests reais ao Peer.
    def exchange(payload):
        with socket.create_connection(("127.0.0.1", a_port), timeout=5) as connection:
            connection.sendall(frame(7, payload, bytes.fromhex(bid), bytes.fromhex(aid)))
            return reply(connection)
    raw = b"abc"
    oid = hashlib.sha256(raw).digest()
    def begin(name, object_id=oid):
        encoded = name.encode()
        return bytes([1]) + object_id + struct.pack("!QQBH", 3, 1, 1, len(encoded)) + encoded
    fields, payload = exchange(begin("invalid.txt"))
    check(fields[1] == 2 and payload == bytes([1, 4]), "receptor rejeita extensão não PDF")
    fields, _ = exchange(begin("wire.pdf"))
    check(fields[1] == 1, "BEGIN independente aceito")
    lz4 = ctypes.CDLL("liblz4.so.1")
    lz4.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lz4.LZ4_compress_default.restype = ctypes.c_int
    compressed = ctypes.create_string_buffer(32)
    size = lz4.LZ4_compress_default(raw, compressed, len(raw), 32)
    body = compressed.raw[:size]
    def chunk(object_id=oid, index=0, offset=0, digest=oid):
        return bytes([2]) + object_id + struct.pack("!QQII", index, offset, len(raw), len(body)) + digest + body
    for malformed in [chunk(digest=bytes(32)), chunk(offset=1), chunk(index=1)]:
        fields, payload = exchange(malformed)
        check(fields[1] == 2 and payload == bytes([1, 4]), "chunk malformado rejeitado na rede")
    with socket.create_connection(("127.0.0.1", a_port), timeout=5) as connection:
        request = frame(7, chunk(), bytes.fromhex(bid), bytes.fromhex(aid))
        connection.sendall(request[:-2])
        connection.shutdown(socket.SHUT_WR)
        check(connection.recv(1) == b"", "desconexão no meio do chunk não recebe ACK")
    fields, _ = exchange(chunk())
    check(fields[1] == 1, "chunk reenviado após desconexão")
    fields, _ = exchange(bytes([3]) + oid)
    check(fields[1] == 1, "COMMIT após retomada publica documento")
    wrong_id = hashlib.sha256(b"xyz").digest()
    exchange(begin("wrong-id.pdf", wrong_id))
    exchange(chunk(object_id=wrong_id))
    fields, payload = exchange(bytes([3]) + wrong_id)
    check(fields[1] == 2 and payload == bytes([1, 4]), "ObjectID final divergente rejeitado na rede")

    bad = root / "not-pdf.txt"
    bad.write_text("existe mas não é PDF")
    output = command(a_port, "upload", bad, "127.0.0.1", a_port, ok=False)
    check("Invalid argument" in output, "extensão rejeitada, sem falso positivo ENOENT")
    existing = root / "download-0.pdf"
    old = existing.read_bytes()
    output = command(b_port, "download", samples[0].name, existing, "127.0.0.1", sp_port, ok=False)
    check("File exists" in output and existing.read_bytes() == old, "destino existente preservado")
    for n in ["one", "two"]:
        folder = root / n
        folder.mkdir()
        path = folder / "ambiguous.pdf"
        path.write_text(n)
        command(a_port, "upload", path, "127.0.0.1", a_port)
    output = command(b_port, "download", "ambiguous.pdf", root / "ambiguous-result.pdf", "127.0.0.1", sp_port, ok=False)
    check("Name not unique" in output, "ambiguidade identificada pelo erro correto")

    command(b_port, "upload", samples[0], "127.0.0.1", b_port)
    stop(a, crash=True)  # Mantém localização obsoleta, sem LEAVE.
    output = command(b_port, "download", samples[0].name, root / "fallback.pdf", "127.0.0.1", sp_port)
    check(f"attempt 1: 127.0.0.1:{a_port}" in output and f"attempt 2: 127.0.0.1:{b_port}" in output, "fallback realmente tentou o primeiro Peer")
    stalled = socket.socket()
    stalled.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    stalled.bind(("127.0.0.1", a_port))
    stalled.listen()
    done = threading.Event()
    def stall():
        connection, _ = stalled.accept()
        with connection:
            done.wait(10)
    thread = threading.Thread(target=stall)
    thread.start()
    started = time.monotonic()
    try:
        output = command(b_port, "download", samples[0].name, root / "stalled-fallback.pdf", "127.0.0.1", sp_port)
        elapsed = time.monotonic() - started
        check(1.5 <= elapsed < 8 and "attempt 2:" in output, "timeout libera worker para próximo Peer")
    finally:
        done.set()
        thread.join(timeout=12)
        stalled.close()
    stop(b)
    stop(sp)
    sp = start_sp("sp-restart")
    a = start_peer("a-restart", a_port)
    b = start_peer("b-restart", b_port)
    check(node_id("a-restart") == aid and node_id("b-restart") == bid, "UUID/NodeID persistem")
    output = command(b_port, "download", samples[-1].name, root / "after-restart.pdf", "127.0.0.1", sp_port)
    check((root / "after-restart.pdf").read_bytes() == samples[-1].read_bytes(), "índice reconstruído após reinício também do Super Peer")
    check("tipo=7" in logs["sp-restart"].read_text(), "novo ANNOUNCE comprovado")
    for process in reversed(processes):
        if process.poll() is None: stop(process)
    check(not any(re.search(r"WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:|LeakSanitizer", log.read_text()) for log in logs.values()), "logs sem diagnósticos dos sanitizadores")
    print(f"RESULTADO: {passed} verificações aprovadas", flush=True)
finally:
    for process in reversed(processes):
        if process.poll() is None:
            try: stop(process)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
    print(f"Evidências: {root}", flush=True)

```

</details>

<a id="script-script-testes-sh"></a>

### script_testes.sh

#### ok

Recebe descrição em $1, imprime PASS e incrementa pass. Não executa o teste: o chamador decidiu que ele passou.

#### notok

Recebe descrição, imprime FAIL e incrementa fail. A falha permanece acumulada até o resultado final.

#### cleanup

Executado por trap. Se NODE_PID existir, envia sinal ao processo iniciado e aguarda sua saída. Não identifica um servidor preexistente que tenha ocupado a mesma porta; daí a importância de usar porta livre.

<details>
<summary>Código completo de script_testes.sh</summary>

```bash
#Universidade Estadual de Mato Grosso do Sul
#Curso de Ciência da Computação
#Professor Dr. Rubens Barbosa Filho
#Ano: 2015.

#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
C1DIR="$ROOT/tests/c1"
NODE="$ROOT/bin/node"
CLIENT="$ROOT/bin/client"
PORT="${C1_PORT:-55101}"
NAME="${C1_NODE_NAME:-C1SP1}"
LOG="$C1DIR/logs/node_${PORT}.log"

mkdir -p "$C1DIR/logs"

pass=0
fail=0

ok() { printf '[PASS] %s\n' "$1"; pass=$((pass+1)); }
notok() { printf '[FAIL] %s\n' "$1"; fail=$((fail+1)); }

cleanup() {
    if [[ -n "${NODE_PID:-}" ]]; then
        kill "$NODE_PID" 2>/dev/null || true
        wait "$NODE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

printf '=== CHECKPOINT C1 — TCP / PROTOCOLO / FRAMING / CRC ===\n'
printf 'Root: %s\nPort: %s\n\n' "$ROOT" "$PORT"

if [[ ! -x "$NODE" || ! -x "$CLIENT" ]]; then
    notok "Executáveis bin/node e bin/client encontrados"
    printf 'Execute: make\n'
    exit 1
fi
ok "compilado"

# 1) Unit tests do protocolo
make -C "$C1DIR" clean >/dev/null 2>&1
if make -C "$C1DIR" >/dev/null 2>&1 && "$C1DIR/test_protocol"; then
    ok "Testes unitários do protocolo"
else
    notok "Testes unitários do protocolo"
fi

# 2) Start server
rm -f "$LOG"
"$NODE" --config "$ROOT/tests/config/c1.conf" --port "$PORT" --name "$NAME" >"$LOG" 2>&1 &
NODE_PID=$!

ready=0
for _ in $(seq 1 30); do
    if "$CLIENT" --cmd ping --host 127.0.0.1 --port "$PORT" >/tmp/c1_ping.out 2>/tmp/c1_ping.err; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" -eq 1 ]]; then
    ok "Servidor TCP aceita conexões"
else
    notok "Servidor TCP aceita conexões"
    cat /tmp/c1_ping.err 2>/dev/null || true
fi

# 3) PING/PONG
if grep -q '^RX PONG' /tmp/c1_ping.out 2>/dev/null && grep -q 'PONG' /tmp/c1_ping.out; then
    ok "PING → PONG"
else
    notok "PING → PONG"
fi

# 4) JOIN/ACK
if "$CLIENT" --cmd join --host 127.0.0.1 --port "$PORT" >/tmp/c1_join.out 2>/tmp/c1_join.err && \
   grep -q '^RX ACK' /tmp/c1_join.out; then
    ok "JOIN → ACK"
else
    notok "JOIN → ACK"
fi

# 5) LEAVE/ACK (sanity test for valid message types)
if "$CLIENT" --cmd leave --host 127.0.0.1 --port "$PORT" >/tmp/c1_leave.out 2>/tmp/c1_leave.err && \
   grep -q '^RX ACK' /tmp/c1_leave.out; then
    ok "LEAVE → ACK"
else
    notok "LEAVE → ACK"
fi

# 6) Concurrent clients
pids=()
for i in $(seq 1 20); do
    "$CLIENT" --cmd ping --host 127.0.0.1 --port "$PORT" >"/tmp/c1_ping_$i.out" 2>"/tmp/c1_ping_$i.err" &
    pids+=("$!")
done
concurrent_ok=1
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then concurrent_ok=0; fi
done
if [[ "$concurrent_ok" -eq 1 ]] && [[ "$(grep -h -c '^RX PONG' /tmp/c1_ping_*.out | awk '{s+=$1} END{print s+0}')" -eq 20 ]]; then
    ok "20 clientes concorrentes"
else
    notok "20 clientes concorrentes"
fi

# 7) Server received all expected frames
rx_count=$(grep -c '^RX PING' "$LOG" 2>/dev/null || true)
if [[ "$rx_count" -ge 21 ]]; then
    ok "Framing/processamento de mensagens no servidor"
else
    notok "Framing/processamento de mensagens no servidor (RX PING=$rx_count)"
fi

# 8) Invalid header version — raw socket test implemented in Python stdlib only
python3 - "$PORT" <<'PY'
import socket, struct, sys
port = int(sys.argv[1])
# version=99; type=PING (3); zero IDs; timestamp=0; payload=0; crc=0
hdr = struct.pack('!BH', 99, 3) + bytes(32+32+16) + bytes(8+4+4)
s = socket.create_connection(('127.0.0.1', port), timeout=2)
s.sendall(hdr)
s.settimeout(2)
try:
    data = s.recv(1)
except Exception:
    data = b''
finally:
    s.close()
# Server is expected to close without accepting the malformed protocol.
sys.exit(0 if data == b'' else 1)
PY
if [[ $? -eq 0 ]]; then
    ok "Rejeição de versão de protocolo inválida"
else
    notok "Rejeição de versão de protocolo inválida"
fi

# 9) Log sanity
if grep -q "Node $NAME started" "$LOG" && grep -q 'NodeID:' "$LOG"; then
    ok "Inicialização e identificação do nó"
else
    notok "Inicialização e identificação do nó"
fi

printf '\n=== RESULTADO C1 ===\n'
printf 'PASS: %d\nFAIL: %d\n' "$pass" "$fail"
if [[ "$fail" -eq 0 ]]; then
    printf 'CHECKPOINT C1: APROVADO\n'
    exit 0
else
    printf 'CHECKPOINT C1: REPROVADO\n'
    printf 'Log do servidor: %s\n' "$LOG"
    exit 1
fi

```

</details>

<a id="script-script-testes-c2-sh"></a>

### script_testes_c2.sh

#### peer_cmd

Encaminha todos os argumentos a bin/peer, acrescentando --local-peer-port com EXECUTOR_PORT. Assim o comando é executado pelo Peer ativo selecionado; mudar o destino remoto não troca esse executor.

#### pass

Imprime aprovação e incrementa PASS.

#### fail

Imprime reprovação e incrementa FAIL; o resultado final do script considera esse acumulador.

#### stop_pid

Recebe PID opcional. Verifica se o processo existe, envia SIGTERM e aguarda a saída. Erros de processo já encerrado são tolerados.

#### cleanup

Encerra os processos criados pelo script. Se houve falha, preserva TMP e informa seu caminho; no sucesso remove apenas o diretório temporário que satisfaz as verificações explícitas do script.

#### wait_port

Tenta conectar via /dev/tcp até 80 vezes, com pausa de 0,05 s. Retorno zero prova que houve aceitação TCP naquele momento; não prova isoladamente JOIN concluído nem identidade do processo.

<details>
<summary>Código completo de script_testes_c2.sh</summary>

```bash
#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
SP_PORT="${C2_SUPERPEER_PORT:-55101}"
PEER_A_PORT="${C2_PEER_A_PORT:-55102}"
PEER_B_PORT="${C2_PEER_B_PORT:-55103}"
TMP="$(mktemp -d)"
SP_PID=""
PEER_A_PID=""
PEER_B_PID=""
EXECUTOR_PORT="$PEER_A_PORT"
peer_cmd() { "$ROOT/bin/peer" --local-peer-port "$EXECUTOR_PORT" "$@"; }
PASS=0
FAIL=0

pass() { printf '[PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf '[FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }

stop_pid() {
    local pid="${1:-}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_pid "$PEER_A_PID"
    EXECUTOR_PORT="$PEER_A_PORT"
    stop_pid "$PEER_B_PID"
    stop_pid "$SP_PID"
    if (( FAIL > 0 )); then
        printf 'Evidências preservadas: %s\n' "$TMP"
    elif [[ -n "$TMP" && "$TMP" == /tmp/tmp.* && -d "$TMP" ]]; then
        rm -rf -- "$TMP"
    fi
}
trap cleanup EXIT INT TERM

wait_port() {
    local port="$1"
    local attempt
    for attempt in $(seq 1 80); do
        if (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

cd "$ROOT" || exit 1
printf '=== CHECKPOINT C2 — ALUNO 1 ===\nRoot: %s\n\n' "$ROOT"

if make -B CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror' >/dev/null; then
    pass 'bin/peer, bin/superpeer, bin/node e bin/client compilados com -Werror'
else
    fail 'compilação'
    exit 1
fi
if make CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror' test-c2 >/dev/null; then
    pass 'CRC, truncamento, payload excessivo, chunks inválidos e ObjectID divergente são rejeitados'
else
    fail 'testes negativos de protocolo e armazenamento'
fi

cd "$TMP" || exit 1
"$ROOT/bin/superpeer" --port "$SP_PORT" --name superpeer >"$TMP/superpeer.log" 2>&1 &
SP_PID=$!
if wait_port "$SP_PORT"; then pass 'Super Peer iniciou'; else fail 'Super Peer iniciou'; exit 1; fi

"$ROOT/bin/peer" serve "$PEER_A_PORT" 127.0.0.1 "$SP_PORT" >"$TMP/peer-a.log" 2>&1 &
PEER_A_PID=$!
"$ROOT/bin/peer" serve "$PEER_B_PORT" 127.0.0.1 "$SP_PORT" >"$TMP/peer-b.log" 2>&1 &
PEER_B_PID=$!
if wait_port "$PEER_A_PORT" && wait_port "$PEER_B_PORT"; then pass 'dois Peers de armazenamento iniciaram e fizeram JOIN'; else fail 'Peers iniciaram'; exit 1; fi

SMALL="$ROOT/trabalho_2026_SD.pdf"
if PEER_TRANSFER_THREADS=4 peer_cmd upload "$SMALL" 127.0.0.1 "$PEER_A_PORT" >"$TMP/upload-small.log" 2>&1 && grep -q 'Upload completed' "$TMP/upload-small.log" && grep -Eq '^ObjectID: [0-9a-f]{64}$' "$TMP/upload-small.log"; then
    pass 'upload de PDF pequeno com ObjectID e saída obrigatória'
else
    fail 'upload de PDF pequeno'
fi
SMALL_ID="$(sed -n 's/^ObjectID: //p' "$TMP/upload-small.log" | head -n 1)"

{
    printf '%%PDF-1.4\n'
    dd if=/dev/zero bs=1048576 count=4 status=none
    dd if=/dev/zero bs=1 count=4096 status=none
    printf '\n%%%%EOF\n'
} >"$TMP/multichunk.pdf"
if PEER_TRANSFER_THREADS=4 peer_cmd upload "$TMP/multichunk.pdf" 127.0.0.1 "$PEER_A_PORT" >"$TMP/upload-large.log" 2>&1 && grep -q '^Chunks: 2$' "$TMP/upload-large.log" && grep -q '^Transfer workers: 2$' "$TMP/upload-large.log"; then
    pass 'PDF multichunk transferido com dois workers configurados'
else
    fail 'upload multichunk concorrente'
fi
LARGE_ID="$(sed -n 's/^ObjectID: //p' "$TMP/upload-large.log" | head -n 1)"
if "$ROOT/bin/peer" benchmark "$TMP/multichunk.pdf" >"$TMP/benchmark.log" 2>&1 && grep -Eq '^Compression throughput: [0-9]+\.[0-9]+ MiB/s$' "$TMP/benchmark.log"; then pass 'benchmark LZ4 informativo executado sem limite rígido'; else fail 'benchmark LZ4'; fi

if peer_cmd download trabalho_2026_SD.pdf "$TMP/by-name.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && cmp -s "$SMALL" "$TMP/by-name.pdf"; then pass 'download por nome e comparação byte a byte'; else fail 'download por nome'; fi
if peer_cmd download "$LARGE_ID" "$TMP/by-id.pdf" 127.0.0.1 "$SP_PORT" >"$TMP/download-id.log" 2>&1 && cmp -s "$TMP/multichunk.pdf" "$TMP/by-id.pdf" && grep -q 'SHA-256 verified' "$TMP/download-id.log"; then pass 'download por ObjectID e SHA-256 final'; else fail 'download por ObjectID'; fi

mkdir -p "$TMP/nome-a" "$TMP/nome-b"
printf '%%PDF-1.4\nobjeto A\n%%%%EOF\n' >"$TMP/nome-a/ambiguo.pdf"
printf '%%PDF-1.4\nobjeto B\n%%%%EOF\n' >"$TMP/nome-b/ambiguo.pdf"
if peer_cmd upload "$TMP/nome-a/ambiguo.pdf" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1 && peer_cmd upload "$TMP/nome-b/ambiguo.pdf" 127.0.0.1 "$PEER_B_PORT" >/dev/null 2>&1 && ! peer_cmd download ambiguo.pdf "$TMP/ambiguo.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1; then pass 'nome ambíguo exige ObjectID'; else fail 'detecção de nome ambíguo'; fi

if peer_cmd upload "$SMALL" 127.0.0.1 "$PEER_B_PORT" >/dev/null 2>&1; then pass 'registro de uma segunda localização para os chunks'; else fail 'segunda localização'; fi
stop_pid "$PEER_A_PID"
PEER_A_PID=""
EXECUTOR_PORT="$PEER_B_PORT"
if peer_cmd download "$SMALL_ID" "$TMP/fallback.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && cmp -s "$SMALL" "$TMP/fallback.pdf"; then pass 'download pela localização remanescente após LEAVE'; else fail 'download pela localização remanescente após LEAVE'; fi

"$ROOT/bin/peer" serve "$PEER_A_PORT" 127.0.0.1 "$SP_PORT" >"$TMP/peer-a-restart.log" 2>&1 &
PEER_A_PID=$!
if wait_port "$PEER_A_PORT"; then
    FIRST_NODE_ID="$(sed -n 's/^NodeID: //p' "$TMP/peer-a.log" | head -n 1)"
    RESTART_NODE_ID="$(sed -n 's/^NodeID: //p' "$TMP/peer-a-restart.log" | head -n 1)"
    if [[ -n "$FIRST_NODE_ID" && "$FIRST_NODE_ID" == "$RESTART_NODE_ID" ]]; then pass 'NodeID do Peer permanece estável após reinicialização'; else fail 'persistência da identidade do Peer'; fi
    EXECUTOR_PORT="$PEER_A_PORT"
    stop_pid "$PEER_B_PID"
    PEER_B_PID=""
    if peer_cmd download "$LARGE_ID" "$TMP/after-restart.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && cmp -s "$TMP/multichunk.pdf" "$TMP/after-restart.pdf"; then pass 'reinicialização e leitura do catálogo local'; else fail 'novo anúncio após reinicialização'; fi
else
    fail 'reinicialização do Peer'
fi

if peer_cmd upload "$SMALL" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1; then pass 'upload repetido idempotente'; else fail 'upload repetido idempotente'; fi
printf 'arquivo de texto existente\\n' >"$TMP/not-pdf.txt"
if ! peer_cmd upload "$TMP/not-pdf.txt" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1; then pass 'arquivo não PDF rejeitado'; else fail 'arquivo não PDF rejeitado'; fi
printf 'conteudo sem assinatura PDF\n' >"$TMP/falso.pdf"
if peer_cmd upload "$TMP/falso.pdf" 127.0.0.1 "$PEER_A_PORT" >/dev/null 2>&1; then pass 'fixture sintética .pdf aceita por extensão'; else fail 'fixture sintética .pdf aceita'; fi
if ! peer_cmd upload "$SMALL" 127.0.0.1 1 >/dev/null 2>&1; then pass 'porta indisponível gera erro'; else fail 'falha de conexão propagada'; fi
if ! peer_cmd download "$SMALL_ID" "$TMP/after-restart.pdf" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1; then pass 'destino existente não é sobrescrito'; else fail 'destino existente não é sobrescrito'; fi
printf 'sentinela de download parcial\n' >"$TMP/owned.part"
if ! peer_cmd download "$SMALL_ID" "$TMP/owned" 127.0.0.1 "$SP_PORT" >/dev/null 2>&1 && grep -qx 'sentinela de download parcial' "$TMP/owned.part"; then pass 'arquivo .part preexistente é preservado'; else fail 'arquivo .part preexistente'; fi

printf '\n=== RESULTADO C2 ===\nPASS: %d\nFAIL: %d\n' "$PASS" "$FAIL"
if (( FAIL == 0 )); then
    printf 'CHECKPOINT C2: APROVADO\n'
    exit 0
fi
printf 'CHECKPOINT C2: REPROVADO\n'
exit 1

```

</details>

<a id="script-script-testes-peer-sh"></a>

### script_testes_peer.sh

#### ok

Imprime e contabiliza um resultado aprovado.

#### notok

Imprime e contabiliza um resultado reprovado.

#### cleanup

Percorre os PIDs registrados pelo script, encerra e aguarda os processos. Não deve ser confundido com apagar o armazenamento dos Peers.

#### wait_for_log

Recebe arquivo e padrão; tenta localizar o padrão até o limite de tentativas, com pausas curtas. Retorna zero quando encontra e não zero ao esgotar.

#### start_peer

Recebe destino de log e argumentos, inicia bin/node com saída ajustada para aparecer prontamente e guarda o PID. Apesar do nome, bin/node é alias de Super Peer, não o armazenamento C2.

<details>
<summary>Código completo de script_testes_peer.sh</summary>

```bash
#!/usr/bin/env bash

# Teste legado de integração usando bin/node, alias de bin/superpeer.
# O Peer de armazenamento do C2 possui outro ponto de entrada em peer.c.
set -u

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PEER="$ROOT/bin/node"
LOG_DIR="$ROOT/tests/peer/logs"
PORT_A="${C1_PEER_PORT:-55201}"
CONCURRENT="${C1_PEER_CONCURRENT:-3}"
PING_CLIENTS="${C1_PEER_PINGS:-20}"

pass=0
fail=0
pids=()

mkdir -p "$LOG_DIR"

ok()
{
    printf '[PASS] %s\n' "$1"
    pass=$((pass + 1))
}

notok()
{
    printf '[FAIL] %s\n' "$1"
    fail=$((fail + 1))
}

cleanup()
{
    local pid

    for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
}

trap cleanup EXIT INT TERM

wait_for_log()
{
    local log_file="$1"
    local pattern="$2"
    local attempt

    for attempt in $(seq 1 50); do
        if grep -Eq -- "$pattern" "$log_file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_peer()
{
    local log_file="$1"
    shift

    stdbuf -oL -eL "$PEER" "$@" >"$log_file" 2>&1 &
    pids+=("$!")
}

printf '=== TESTE DE INTEGRACAO — PEER COM PEER ===\n'
printf 'Root: %s\nPorta inicial: %s\nPeers concorrentes: %s\nPINGs concorrentes: %s\n\n' "$ROOT" "$PORT_A" "$CONCURRENT" "$PING_CLIENTS"

if ! make -C "$ROOT" bin/node >/dev/null; then
    notok 'compilação do peer'
    exit 1
fi
ok 'peer compilado (sem executar bin/client)'

if make -C "$ROOT/tests/c1" >/dev/null 2>&1 && "$ROOT/tests/c1/test_protocol"; then
    ok 'teste unitário do protocolo'
else
    notok 'teste unitário do protocolo'
fi

LOG_A="$LOG_DIR/peer_a_${PORT_A}.log"
rm -f "$LOG_A"
start_peer "$LOG_A" --config "$ROOT/tests/config/c1.conf" --port "$PORT_A" --name A

if wait_for_log "$LOG_A" '^Node A started$'; then
    ok 'Peer A iniciou o servidor TCP'
else
    notok 'Peer A iniciou o servidor TCP'
fi

if grep -Eq '^NodeID: [0-9a-f]{64}$' "$LOG_A" 2>/dev/null; then
    ok 'Peer A exibiu um NodeID de 32 bytes'
else
    notok 'Peer A exibiu um NodeID de 32 bytes'
fi

PING_OUT="$LOG_DIR/ping.out"
PING_ERR="$LOG_DIR/ping.err"
if "$PEER" --cmd ping --host 127.0.0.1 --port "$PORT_A" >"$PING_OUT" 2>"$PING_ERR" && grep -q '^TX PING$' "$PING_OUT" && grep -q '^RX PONG$' "$PING_OUT"; then
    ok 'PING com payload textual recebeu PONG textual'
else
    notok 'PING com payload textual recebeu PONG textual'
fi

SHORT_PING_OUT="$LOG_DIR/ping_short.out"
SHORT_PING_ERR="$LOG_DIR/ping_short.err"
if "$PEER" -c ping -h 127.0.0.1 -p "$PORT_A" >"$SHORT_PING_OUT" 2>"$SHORT_PING_ERR" && grep -q '^TX PING$' "$SHORT_PING_OUT" && grep -q '^RX PONG$' "$SHORT_PING_OUT"; then
    ok 'opções curtas -c, -h e -p processadas por getopt_long'
else
    notok 'opções curtas -c, -h e -p processadas por getopt_long'
fi

LEAVE_OUT="$LOG_DIR/leave.out"
LEAVE_ERR="$LOG_DIR/leave.err"
if "$PEER" --cmd leave --host 127.0.0.1 --port "$PORT_A" >"$LEAVE_OUT" 2>"$LEAVE_ERR" && grep -q '^TX LEAVE$' "$LEAVE_OUT" && grep -q '^RX ACK$' "$LEAVE_OUT"; then
    ok 'LEAVE recebeu ACK'
else
    notok 'LEAVE recebeu ACK'
fi

if [[ "$PING_CLIENTS" =~ ^[1-9][0-9]*$ ]]; then
    rm -f "$LOG_DIR"/ping_concurrent_*.out "$LOG_DIR"/ping_concurrent_*.err
    command_pids=()
    index=1
    while [[ "$index" -le "$PING_CLIENTS" ]]; do
        "$PEER" --cmd ping --host 127.0.0.1 --port "$PORT_A" >"$LOG_DIR/ping_concurrent_${index}.out" 2>"$LOG_DIR/ping_concurrent_${index}.err" &
        command_pids+=("$!")
        index=$((index + 1))
    done

    concurrent_ping_ok=1
    for pid in "${command_pids[@]}"; do
        if ! wait "$pid"; then
            concurrent_ping_ok=0
        fi
    done
    pong_count=$(grep -h -c '^RX PONG$' "$LOG_DIR"/ping_concurrent_*.out | awk '{sum += $1} END {print sum + 0}')
    if [[ "$concurrent_ping_ok" -eq 1 && "$pong_count" -eq "$PING_CLIENTS" ]]; then
        ok "Peer A respondeu a ${PING_CLIENTS} PINGs concorrentes"
    else
        notok "Peer A respondeu a ${PING_CLIENTS} PINGs concorrentes (PONGs=$pong_count)"
    fi

    expected_pings=$((PING_CLIENTS + 2))
    if wait_for_log "$LOG_A" "^RX PING$" && [[ "$(grep -c '^RX PING$' "$LOG_A" 2>/dev/null || true)" -ge "$expected_pings" ]]; then
        ok 'framing dos PINGs concorrentes no servidor'
    else
        notok 'framing dos PINGs concorrentes no servidor'
    fi
else
    notok 'valor de C1_PEER_PINGS'
fi

PORT_B=$((PORT_A + 1))
LOG_B="$LOG_DIR/peer_b_${PORT_B}.log"
rm -f "$LOG_B"

# A forma posicional legada em superpeer_app.c inicia o servidor local e envia JOIN.
start_peer "$LOG_B" "$PORT_B" 127.0.0.1 "$PORT_A"

if wait_for_log "$LOG_A" 'JOIN validado:.*membros=2'; then
    ok 'Peer A recebeu, validou e registrou o JOIN de Peer B'
else
    notok 'Peer A recebeu, validou e registrou o JOIN de Peer B'
fi

if wait_for_log "$LOG_B" 'JOIN aceito pelo peer remoto:.*membros=2'; then
    ok 'Peer B recebeu ACK e registrou Peer A'
else
    notok 'Peer B recebeu ACK e registrou Peer A'
fi

if grep -Eq '^NodeID: [0-9a-f]{64}$' "$LOG_B" 2>/dev/null; then
    ok 'Peer B exibiu um NodeID de 32 bytes'
else
    notok 'Peer B exibiu um NodeID de 32 bytes'
fi

if [[ "$CONCURRENT" =~ ^[1-9][0-9]*$ ]]; then
    index=1
    while [[ "$index" -le "$CONCURRENT" ]]; do
        port=$((PORT_A + 1 + index))
        log_file="$LOG_DIR/peer_${index}_${port}.log"
        rm -f "$log_file"
        start_peer "$log_file" "$port" 127.0.0.1 "$PORT_A"
        index=$((index + 1))
    done

    expected_members=$((2 + CONCURRENT))
    if wait_for_log "$LOG_A" "JOIN validado:.*membros=${expected_members}"; then
        ok "Peer A processou ${CONCURRENT} JOINs concorrentes"
    else
        notok "Peer A processou ${CONCURRENT} JOINs concorrentes"
    fi
else
    notok 'valor de C1_PEER_CONCURRENT'
fi

join_count=$(grep -c '^JOIN validado:' "$LOG_A" 2>/dev/null || true)
if [[ "$join_count" -ge $((CONCURRENT + 1)) ]]; then
    ok 'framing e processamento de mensagens entre peers'
else
    notok "framing e processamento de mensagens entre peers (JOINs=$join_count)"
fi

printf '\n=== RESULTADO PEER COM PEER ===\n'
printf 'PASS: %d\nFAIL: %d\n' "$pass" "$fail"

if [[ "$fail" -eq 0 ]]; then
    printf 'TESTE PEER: APROVADO\n'
    exit 0
fi

printf 'TESTE PEER: REPROVADO\n'
printf 'Logs: %s\n' "$LOG_DIR"
exit 1

```

</details>

<a id="script-redvidassobretrabalhodepd-common-sh"></a>

### redvidassobretrabalhodepd/common.sh

#### log

Imprime informação com prefixo TEST; não altera PASS/FAIL.

#### ok

Imprime PASS com cor e incrementa o contador de aprovações.

#### fail

Imprime FAIL com cor e incrementa o contador de falhas.

#### section

Imprime um título separador entre grupos de verificações.

#### cleanup

Percorre PIDS e envia sinal aos processos registrados, tolerando os já encerrados. Esta variante não executa wait para cada processo.

#### require_bin

Verifica -x no caminho fornecido: existência e permissão de execução. Se falhar, contabiliza falha e retorna não zero. Não comprova que o binário foi recompilado com os fontes atuais.

#### wait_for_pattern

Recebe log, expressão e prazo em segundos. Consulta grep repetidamente e compara o tempo decorrido até achar o marcador ou esgotar o limite.

#### assert_file_equal

Recebe dois caminhos e usa cmp -s para comparação byte a byte. Registra PASS/FAIL, não apenas igualdade de nome ou tamanho.

#### assert_contains

Recebe arquivo, expressão regular e descrição. Registra resultado de grep -Eq; texto presente é evidência limitada ao que o padrão realmente verifica.

#### assert_no_crash

Procura strings típicas de crash ou erro de memória/deadlock. Ausência dessas palavras é uma checagem de log, não prova de ausência de travamento, vazamento ou corrida.

#### summary

Imprime PASS/FAIL e termina com uma expressão que retorna sucesso apenas se FAIL for zero. Esse status pode ser usado como resultado final do runner.

<details>
<summary>Código completo de redvidassobretrabalhodepd/common.sh</summary>

```bash
# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash
set -u

source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

PASS=0
FAIL=0
PIDS=()

log()  { printf '[TEST] %s\n' "$*"; }
ok()   { printf '\033[32m[PASS]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
fail() { printf '\033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
section() { printf '\n==== %s ====\n' "$*"; }

cleanup() {
    for p in "${PIDS[@]:-}"; do
        kill "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

require_bin() {
    local b="$1"
    if [[ ! -x "$b" ]]; then
        fail "Executável não encontrado: $b"
        return 1
    fi
}

wait_for_pattern() {
    local file="$1" pattern="$2" timeout="$3"
    local start now
    start=$(date +%s)
    while true; do
        grep -Eq "$pattern" "$file" 2>/dev/null && return 0
        now=$(date +%s)
        (( now - start >= timeout )) && return 1
        sleep 1
    done
}

assert_file_equal() {
    local a="$1" b="$2"
    if cmp -s "$a" "$b"; then
        ok "Arquivos são idênticos: $(basename "$a")"
    else
        fail "Arquivos diferem: $a x $b"
    fi
}

assert_contains() {
    local file="$1" pattern="$2" desc="$3"
    if grep -Eq "$pattern" "$file"; then
        ok "$desc"
    else
        fail "$desc — padrão não encontrado: $pattern"
    fi
}

assert_no_crash() {
    local file="$1"
    if grep -Eiq 'segmentation fault|core dumped|double free|heap-buffer-overflow|deadlock' "$file"; then
        fail "Falha de execução detectada em $(basename "$file")"
    else
        ok "Sem crash/deadlock reportado em $(basename "$file")"
    fi
}

summary() {
    echo
    echo "=============================="
    echo "PASS: $PASS"
    echo "FAIL: $FAIL"
    echo "=============================="
    [[ "$FAIL" -eq 0 ]]
}

```

</details>

<a id="script-redvidassobretrabalhodepd-env-sh"></a>

### redvidassobretrabalhodepd/env.sh

Não define funções. Configura PROJECT_ROOT com possibilidade de override; a expressão com BASH_SOURCE[0] calcula a raiz a partir da localização deste arquivo, não do diretório em que você digitou bash. Define caminhos de binários, fixtures, configuração e logs usados por common.sh/run.sh. source executa essas atribuições no shell chamador.

<details>
<summary>Código completo de redvidassobretrabalhodepd/env.sh</summary>

```bash
# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash

# Ajuste esta variável para a raiz do projeto.
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

NODE_BIN="${NODE_BIN:-$PROJECT_ROOT/bin/node}"
CLIENT_BIN="${CLIENT_BIN:-$PROJECT_ROOT/bin/client}"

TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$TEST_ROOT/data"
CONFIG_DIR="$TEST_ROOT/config"
if [[ ! -f "$CONFIG_DIR/c1.conf" && -f "$PROJECT_ROOT/tests/config/c1.conf" ]]; then
    CONFIG_DIR="$PROJECT_ROOT/tests/config"
fi
LOG_DIR="${LOG_DIR:-/tmp/bittorrent-tests}"

# Contrato temporal do PDF
HEARTBEAT_SEC="${HEARTBEAT_SEC:-5}"
FAILURE_TIMEOUT_SEC="${FAILURE_TIMEOUT_SEC:-15}"

# O PDF exige chunking de 4 MB.
CHUNK_SIZE="${CHUNK_SIZE:-4194304}"

# Quantidade de nós usada nos testes
SUPERPEERS="${SUPERPEERS:-5}"
PEERS="${PEERS:-3}"

export PROJECT_ROOT NODE_BIN CLIENT_BIN TEST_ROOT DATA_DIR CONFIG_DIR LOG_DIR
export HEARTBEAT_SEC FAILURE_TIMEOUT_SEC CHUNK_SIZE SUPERPEERS PEERS

mkdir -p "$LOG_DIR"

```

</details>

<a id="script-redvidassobretrabalhodepd-run-sh"></a>

### redvidassobretrabalhodepd/run.sh

Não define funções próprias: importa common.sh. Localiza fixtures, enumera PDFs, cria diretório de evidências, inicia Super Peer e Peer, espera marcadores e faz upload/download de cada nome com comparação byte a byte. A enumeração inclui PDFs da raiz do projeto, inclusive cópias baixadas manualmente. Com o modelo atual, dois nomes para bytes idênticos têm o mesmo ObjectID e preservam o primeiro nome; portanto essa expectativa de download por cada nome pode falhar sem corrupção de dados. O log combinado também mistura redirecionamento inicial e append de vários processos; prefira logs separados ao investigar detalhes de concorrência.

<details>
<summary>Código completo de redvidassobretrabalhodepd/run.sh</summary>

```bash
# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

section "C2 — arquivo, SHA-256, chunks e LZ4"
require_bin "$NODE_BIN" || exit 1
require_bin "$CLIENT_BIN" || exit 1
if [[ -x "$TEST_ROOT/generate_fixtures.sh" ]]; then
    "$TEST_ROOT/generate_fixtures.sh" >/dev/null || exit 1
elif [[ ! -d "$DATA_DIR" && -f "$TEST_ROOT/data.tar.gz" ]]; then
    tar -xzf "$TEST_ROOT/data.tar.gz" -C "$TEST_ROOT" || exit 1
fi

PDFS=()
for f in "$DATA_DIR"/*.pdf "$PROJECT_ROOT"/*.pdf; do
    [[ -f "$f" ]] || continue
    PDFS+=("$f")
done
if [[ "${#PDFS[@]}" -eq 0 ]]; then
    fail "Nenhum arquivo .pdf encontrado em $DATA_DIR ou $PROJECT_ROOT"
    summary
    exit 1
fi

RUN_DIR="$(mktemp -d "$LOG_DIR/c2-pdf.XXXXXX")" || exit 1
LOG="$RUN_DIR/c2.log"
DOWNLOAD_DIR="$RUN_DIR/downloads"
mkdir -p "$DOWNLOAD_DIR"

SUPERPEER_PORT="${C2_PORT:-55301}"
PEER_PORT="${C2_PEER_PORT:-55302}"
cd "$PROJECT_ROOT" || exit 1
"$NODE_BIN" --config "$CONFIG_DIR/c1.conf" --port "$SUPERPEER_PORT" --name C2SP1 >"$LOG" 2>&1 &
PIDS+=("$!")
if ! wait_for_pattern "$LOG" '^Node C2SP1 started$' 10; then
    fail "Super Peer não iniciou; consulte $LOG"
    summary
    exit 1
fi

"$CLIENT_BIN" serve "$PEER_PORT" 127.0.0.1 "$SUPERPEER_PORT" >>"$LOG" 2>&1 &
PIDS+=("$!")
if ! wait_for_pattern "$LOG" '^Peer storage started$' 10; then
    fail "Peer de armazenamento não iniciou; consulte $LOG"
    summary
    exit 1
fi

for f in "${PDFS[@]}"; do
    name="$(basename "$f")"
    if "$CLIENT_BIN" --local-peer-port "$PEER_PORT" upload "$f" 127.0.0.1 "$PEER_PORT" >>"$LOG" 2>&1; then
        ok "Upload de $name"
    else
        fail "Upload de $name"
        continue
    fi
    if "$CLIENT_BIN" --local-peer-port "$PEER_PORT" download "$name" "$DOWNLOAD_DIR/$name" 127.0.0.1 "$SUPERPEER_PORT" >>"$LOG" 2>&1; then
        ok "Download de $name"
        assert_file_equal "$f" "$DOWNLOAD_DIR/$name"
    else
        fail "Download de $name"
    fi
done

assert_contains "$LOG" 'SHA-256|ObjectID' "Há evidência de SHA-256/ObjectID"
assert_contains "$LOG" 'LZ4|compress' "Há evidência de compressão LZ4"
assert_contains "$LOG" 'chunk|Chunk' "Há evidência de fragmentação"
assert_no_crash "$LOG"

printf 'Log: %s\n' "$LOG"
summary

```

</details>

<a id="script-redvidassobretrabalhodepd-run-pre-refactor-sh"></a>

### redvidassobretrabalhodepd/run.pre-refactor.sh

Cópia histórica já adaptada, preservada para referência; não é o runner recomendado do código atual. Filtra assinatura %PDF e não informa --local-peer-port nos comandos, diferentemente de run.sh. Não representa um original intocado do professor.

<details>
<summary>Código completo de redvidassobretrabalhodepd/run.pre-refactor.sh</summary>

```bash
# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015

#!/usr/bin/env bash
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

section "C2 — arquivo, SHA-256, chunks e LZ4"
require_bin "$NODE_BIN" || exit 1
require_bin "$CLIENT_BIN" || exit 1
if [[ -x "$TEST_ROOT/generate_fixtures.sh" ]]; then
    "$TEST_ROOT/generate_fixtures.sh" >/dev/null || exit 1
elif [[ ! -d "$DATA_DIR" && -f "$TEST_ROOT/data.tar.gz" ]]; then
    tar -xzf "$TEST_ROOT/data.tar.gz" -C "$TEST_ROOT" || exit 1
fi

PDFS=()
for f in "$DATA_DIR"/*.pdf "$PROJECT_ROOT"/*.pdf; do
    [[ -f "$f" ]] || continue
    if head -c 1024 "$f" | LC_ALL=C grep -a -q '%PDF-'; then
        PDFS+=("$f")
    else
        log "Ignorando $(basename "$f"): não contém assinatura PDF nos primeiros 1024 bytes"
    fi
done
if [[ "${#PDFS[@]}" -eq 0 ]]; then
    fail "Nenhum PDF válido encontrado em $DATA_DIR ou $PROJECT_ROOT"
    summary
    exit 1
fi

RUN_DIR="$(mktemp -d "$LOG_DIR/c2-pdf.XXXXXX")" || exit 1
LOG="$RUN_DIR/c2.log"
DOWNLOAD_DIR="$RUN_DIR/downloads"
mkdir -p "$DOWNLOAD_DIR"

SUPERPEER_PORT="${C2_PORT:-55301}"
PEER_PORT="${C2_PEER_PORT:-55302}"
cd "$PROJECT_ROOT" || exit 1
"$NODE_BIN" --config "$CONFIG_DIR/c1.conf" --port "$SUPERPEER_PORT" --name C2SP1 >"$LOG" 2>&1 &
PIDS+=("$!")
if ! wait_for_pattern "$LOG" '^Node C2SP1 started$' 10; then
    fail "Super Peer não iniciou; consulte $LOG"
    summary
    exit 1
fi

"$CLIENT_BIN" serve "$PEER_PORT" 127.0.0.1 "$SUPERPEER_PORT" >>"$LOG" 2>&1 &
PIDS+=("$!")
if ! wait_for_pattern "$LOG" '^Peer storage started$' 10; then
    fail "Peer de armazenamento não iniciou; consulte $LOG"
    summary
    exit 1
fi

for f in "${PDFS[@]}"; do
    name="$(basename "$f")"
    if "$CLIENT_BIN" upload "$f" 127.0.0.1 "$PEER_PORT" >>"$LOG" 2>&1; then
        ok "Upload de $name"
    else
        fail "Upload de $name"
        continue
    fi
    if "$CLIENT_BIN" download "$name" "$DOWNLOAD_DIR/$name" 127.0.0.1 "$SUPERPEER_PORT" >>"$LOG" 2>&1; then
        ok "Download de $name"
        assert_file_equal "$f" "$DOWNLOAD_DIR/$name"
    else
        fail "Download de $name"
    fi
done

assert_contains "$LOG" 'SHA-256|ObjectID' "Há evidência de SHA-256/ObjectID"
assert_contains "$LOG" 'LZ4|compress' "Há evidência de compressão LZ4"
assert_contains "$LOG" 'chunk|Chunk' "Há evidência de fragmentação"
assert_no_crash "$LOG"

printf 'Log: %s\n' "$LOG"
summary

```

</details>

## 28. Headers, tipos e regras de compilação

Um protótipo não é uma segunda função implementada. A definição está no catálogo do .c correspondente; as funções inline de wire.h/remote_error.h já foram explicadas individualmente. Include guards evitam repetir declarações numa mesma unidade de tradução. Headers expõem o que o chamador precisa saber; detalhes privados ficam no .c.


### app_config.h

AppConfig centraliza IP anunciado, bind, endpoint do Super Peer, portas e diretório de dados. A variável extern é definida uma única vez em app_config.c.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef APP_CONFIG_H
#define APP_CONFIG_H
#include "node.h"
typedef struct
{
    char advertised[NODE_ADDRESS_SIZE];
    char bind_ip[NODE_ADDRESS_SIZE];
    char superpeer[NODE_ADDRESS_SIZE];
    char data_dir[512];
    uint16_t port, superpeer_port;
} AppConfig;
extern AppConfig app_config;
int app_config_load(int *argc, char **argv, int superpeer);
int app_identity(const char *directory, NodeConfig *config, uint16_t port);
#endif

```

</details>

### common.h

Constantes comuns de NodeID, UUID e endereço textual, evitando números duplicados entre camadas.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>

/* Constantes compartilhadas entre a identidade do no e o protocolo. */
#define NODE_ID_SIZE 32U /* Resultado binário do SHA-256, compartilhado com o header de rede. */
#define NODE_UUID_SIZE 16U /* Identificador da instância usado na formação do NodeID. */
#define NODE_ADDRESS_SIZE INET6_ADDRSTRLEN /* Espaço para IP textual IPv6 e terminador zero. */

#endif /* COMMON_H */

```

</details>

### compression.h

Contrato para buffers LZ4: compressão/descompressão alocam saída; cabe ao chamador liberar.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef COMPRESSION_H
#define COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

#define COMPRESSION_LZ4 1U

/* Camada LZ4: os buffers retornados pertencem ao chamador e devem ser liberados com free(). */
int compression_lz4_bound(size_t input_size, size_t *bound);
int compression_lz4_compress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size);
int compression_lz4_decompress(const uint8_t *input, size_t input_size, size_t expected_size, uint8_t **output);

#endif

```

</details>

### concurrent_server.h

Define a assinatura do callback de conexão e operações de ciclo de vida. O runtime é dono do listener após criação bem-sucedida e gerencia o fechamento das conexões atendidas.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef CONCURRENT_SERVER_H
#define CONCURRENT_SERVER_H

typedef struct ConcurrentServer ConcurrentServer;
typedef void (*ConcurrentServerHandler)(void *context, int client_fd);

int concurrent_server_create(int server_fd, ConcurrentServerHandler handler, void *context, ConcurrentServer **output);
int concurrent_server_run(ConcurrentServer *server);
void concurrent_server_stop(ConcurrentServer *server);
void concurrent_server_destroy(ConcurrentServer *server);

#endif

```

</details>

### content.h

Helpers de hash, extensão, nome e sincronização do diretório pai. Aceitar .pdf não significa validar sua estrutura interna.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef CONTENT_H
#define CONTENT_H

#include "metadata.h"

#include <stddef.h>
#include <stdint.h>

int content_sha256(const uint8_t *data, size_t size, uint8_t digest[OBJECT_ID_SIZE]);
int content_validate_pdf(const char *path);
int content_pdf_name(const char *path);
int content_sync_parent(const char *path);
int content_basename(const char *path, char output[METADATA_NAME_SIZE]);

#endif

```

</details>

### directory.h

API adaptadora entre tabela de documentos e tabela de membros; não é armazenamento de bytes.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "metadata.h"
#include "superpeer.h"
#include "transfer_protocol.h"

typedef struct Directory Directory;

int directory_create(MetadataStore *metadata, SuperPeer *superpeer, Directory **output);
void directory_destroy(Directory *directory);
int directory_announce(Directory *directory, const TransferDocument *document, const MetadataChunk *chunks, const NodeID *owner);
int directory_lookup(Directory *directory, TransferSelectorType type, const ObjectID *id, const char *name, TransferLookupResult *result);

#endif

```

</details>

### file_client.h

FileSession transmite identidade do executor, stream de progresso e cwd da CLI. Não é um cadastro independente de nó.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef FILE_CLIENT_H
#define FILE_CLIENT_H

#include <stdint.h>
#include <stdio.h>
#include "node.h"

typedef struct
{
    NodeID source;
    FILE *output;
    const char *directory;
} FileSession;

int file_client_upload(const FileSession *session, const char *path, const char *peer_host, uint16_t peer_port);
int file_client_download(const FileSession *session, const char *selector, const char *destination, const char *superpeer_host, uint16_t superpeer_port);
int file_client_benchmark_lz4(const char *path);

#endif

```

</details>

### local_control.h

Expõe o tipo opaco LocalControl e as operações de iniciar, parar e emitir comando. O chamador não manipula seus mutexes/descritores internos.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef LOCAL_CONTROL_H
#define LOCAL_CONTROL_H
#include "node.h"
#include <stdint.h>
typedef struct LocalControl LocalControl;
int local_control_start(uint16_t port, const NodeID *source, LocalControl **output);
void local_control_stop(LocalControl *control);
int local_control_command(uint16_t local_port, int upload, const char *file, const char *destination, const char *host, uint16_t remote_port);
#endif

```

</details>

### metadata.h

MetadataDocument acrescenta versão e proprietário completo. MetadataChunk contém descritor sem bytes. ObjectID binário tem 32 bytes; representação textual exige 65 incluindo NUL. METADATA_CHUNK_SIZE vale 4 MiB.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef METADATA_H
#define METADATA_H

#include "node.h"

#define OBJECT_ID_SIZE 32U
#define OBJECT_ID_HEX_SIZE 65U
#define METADATA_NAME_SIZE 256U
/* Convenção de integração: 4 MiB de conteúdo original por chunk. */
#define METADATA_CHUNK_SIZE UINT64_C(4194304)

typedef struct { uint8_t bytes[OBJECT_ID_SIZE]; } ObjectID;
typedef struct MetadataStore MetadataStore;
typedef struct
{
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    uint64_t file_size;
    uint64_t chunk_count;
    uint64_t version;
    NodeID owner;
    uint8_t compression;
} MetadataDocument;

typedef struct
{
    uint64_t index, offset;
    uint32_t raw_size, compressed_size;
    uint8_t hash[OBJECT_ID_SIZE];
} MetadataChunk;

int metadata_announce(MetadataStore *store, const MetadataDocument *document, const MetadataChunk *chunks, const NodeID *owner);
int metadata_find_name(MetadataStore *store, const char *name, ObjectID *id);
int metadata_chunk_descriptor(MetadataStore *store, const ObjectID *id, uint64_t index, MetadataChunk *output);
int metadata_remove_peer(MetadataStore *store, const NodeID *peer);

/* Retornos: 0 em sucesso, -1 com errno em erro. Saídas preservadas em erro, salvo indicação. */
/* SHA-256 em leitura incremental; o chamador deve impedir alterações no arquivo durante a leitura. */
int object_id_file(const char *path, ObjectID *output, uint64_t *file_size);
int object_id_to_hex(const ObjectID *id, char *output, size_t capacity);
/* Cada instância é independente; create zera output em erro. */
int metadata_create(MetadataStore **output);
/* Encerrar e aguardar todas as threads usuárias antes de destruir. */
void metadata_destroy(MetadataStore *store);
/* Registro idempotente por ID/tamanho; nomes alternativos preservam o primeiro nome. Tamanho divergente: EEXIST. */
int metadata_register_document(MetadataStore *store, const ObjectID *id, const char *name, uint64_t file_size);
int metadata_find_document(MetadataStore *store, const ObjectID *id, MetadataDocument *output);
int metadata_remove_document(MetadataStore *store, const ObjectID *id);
/* Índice começa em zero. Registro repetido do mesmo peer é idempotente. Não valida membership. */
int metadata_register_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer);
int metadata_unregister_chunk(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, const NodeID *peer);
/* Retorna cópia consistente alocada; liberar com free(). Sem peers: NULL/0. Ordem não definida. */
int metadata_chunk_peers(MetadataStore *store, const ObjectID *id, uint64_t chunk_index, NodeID **output, size_t *count);

#endif

```

</details>

### network.h

Usa int para descritores, uint16_t para portas, size_t para quantidades e ssize_t para I/O com possibilidade de -1.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Cria servidor TCP IPv4 em todas as interfaces; configura reutilização de endereço, bind e fila listen. */
int network_create_server(uint16_t porta, int backlog);
/* Aceita uma conexão e retorna seu descritor; erros, inclusive EINTR, são tratados pelo chamador. */
int network_accept_client(int server_fd);
/* Conecta a um IPv4 numérico e porta; fecha o socket se a validação ou conexão falhar. */
int network_connect(const char *ip, uint16_t porta);
/* Repete send para completar o buffer e retoma após EINTR; retorna total enviado ou -1. */
ssize_t network_send_all(int sock, const void *buffer, size_t tam);
/* Acumula recv até o tamanho pedido; retorna total parcial no fechamento, zero sem dados ou -1 em erro. */
ssize_t network_recv_exact(int sock, void *buffer, size_t tam);
/* Tenta encerrar os dois sentidos e sempre chama close; o retorno reflete o resultado de close. */
int network_shutdown(int sock);

#endif /* NETWORK_H */

```

</details>

### node.h

NodeConfig contém IP, porta e UUID; Node reúne config, NodeID, PID e papel. Validação não implica autenticação.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef NODE_H
#define NODE_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NODE_ID_HEX_SIZE ((NODE_ID_SIZE * 2U) + 1U)
#define NODE_UUID_HEX_SIZE ((NODE_UUID_SIZE * 2U) + 1U)

typedef struct
{
    uint8_t bytes[NODE_ID_SIZE];
} NodeID;

typedef struct
{
    char ip[NODE_ADDRESS_SIZE];
    uint16_t port;
    uint8_t uuid[NODE_UUID_SIZE];
} NodeConfig;

typedef enum
{
    NODE_ROLE_PEER = 0,
    NODE_ROLE_SUPERPEER = 1
} NodeRole;

typedef struct
{
    NodeID id;
    NodeConfig config;
    pid_t process_id;
    NodeRole role;
} Node;

// Inicialização de configuração com IP, porta e UUID gerado aleatoriamente.
int node_config_init(NodeConfig *config, const char *ip, uint16_t port);

// Inicialização de configuração com IP, porta e UUID fornecido pelo chamador.
int node_config_init_with_uuid(NodeConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE]);

// Gera um UUID aleatório de 16 bytes, retornando zero em sucesso e -1 em erro.
int node_generate_uuid(uint8_t uuid[NODE_UUID_SIZE]);

// Valida a configuração do nó, incluindo IP, porta e UUID.
int node_config_validate(const NodeConfig *config);

// Calcula o NodeID a partir da configuração do nó, usando SHA-256 de IP binário + porta em ordem de rede + UUID.
int node_compute_id(const NodeConfig *config, NodeID *id);

// Inicializa o nó com a configuração fornecida, calculando o NodeID e registrando o PID.
int node_init(Node *node, const NodeConfig *config);

// Valida o nó, verificando consistência do NodeID, PID e papel; não autentica.
int node_validate(const Node *node);

// Converte o NodeID binário em uma string hexadecimal; retorna -1 em erro.
int node_id_to_hex(const NodeID *id, char *output, size_t output_size);

// Converte uma string hexadecimal em NodeID binário; retorna -1 em erro.
int node_id_from_hex(NodeID *id, const char *hex);

// Compara dois NodeIDs lexicograficamente; retorna -1, 0 ou 1.
int node_id_compare(const NodeID *left, const NodeID *right);

// Compara dois NodeIDs para igualdade; ponteiros nulos não são considerados iguais.
int node_id_equal(const NodeID *left, const NodeID *right);

// Retorna o PID armazenado no nó; retorna -1 se o nó for nulo.
pid_t node_get_process_id(const Node *node);

#endif

```

</details>

### peer_service.h

Ponto de entrada da execução prolongada do Peer de armazenamento.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef PEER_SERVICE_H
#define PEER_SERVICE_H

#include <stdint.h>

int peer_service_run(uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port);

#endif

```

</details>

### protocol.h

Define Message e header em memória, tamanho wire 98, códigos de mensagem e limite de 5 MiB. O enum ocupa um tipo de compilador, mas message_type é serializado em um byte; os dois conceitos não devem ser confundidos.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

/* Versão aceita, tamanho do identificador de transação e limite de alocação por payload. */
#define PROTOCOL_VERSION 1u
#define TRANSACTION_ID_SIZE 16u
#define MAX_PAYLOAD_SIZE (5u * 1024u * 1024u)

/*
 * Layout do payload de JOIN e ACK usado na integracao:
 * [46 bytes de IP textual, incluindo terminador NUL e padding]
 * [2 bytes de porta em ordem de rede]
 * [16 bytes de UUID]
 */
#define JOIN_PAYLOAD_WIRE_SIZE (NODE_ADDRESS_SIZE + 2u + NODE_UUID_SIZE)

/*
 * O header e serializado manualmente. Nao use sizeof(Header) para
 * determinar o tamanho enviado pela rede, pois a struct pode ter padding.
 */
#define HEADER_WIRE_SIZE 98u

/* Códigos de mensagem transmitidos em um byte no header. */
typedef enum m_type
{
    M_JOIN = 0,
    M_ACK = 1,
    M_ERROR = 2,
    M_PING = 3,
    M_PONG = 4,
    M_LEAVE = 5,
    M_LOOKUP = 6,
    M_STORE = 7,
    M_DOWNLOAD_REQ = 8,
    M_DOWNLOAD_REP = 9,
    M_PREPARE = 10,
    M_COMMIT = 11,
    M_ABORT = 12,
    M_HEARTBEAT = 13,
    M_GOSSIP = 14,
    M_ELECTION = 15,
    M_OK = 16,
    M_COORDINATOR = 17,
    M_SNAPSHOT = 18,
    M_STATE_TRANSFER = 19,
} Message_Type;

/* Representação em memória: a serialização define a disposição dos campos na rede. */
typedef struct header
{
    uint8_t protocol_version;
    uint8_t message_type;
    uint8_t source_node[NODE_ID_SIZE]; /* Identidade de quem envia a mensagem. */
    uint8_t destination_node[NODE_ID_SIZE]; /* Destinatário; JOIN inicial pode usar zeros. */
    uint8_t transaction_id[TRANSACTION_ID_SIZE]; /* Relaciona solicitação e resposta. */
    uint64_t timestamp;
    uint32_t payload_size; /* Delimita o corpo no fluxo contínuo de bytes do TCP. */
    uint32_t checksum; /* CRC32 do header com este campo zerado, seguido do corpo. */
} Header;

/* O payload alocado pertence à mensagem e deve ser liberado com message_free. */
typedef struct message
{
    Header header;
    uint8_t *payload;
} Message;

/* Distingue sucesso, erro e fechamento antes do início de uma nova mensagem. */
typedef enum protocol_result
{
    PROTOCOL_ERROR = -1,
    PROTOCOL_OK = 0,
    PROTOCOL_CLOSED = 1,
} Protocol_Result;

/* Zera a mensagem e define a versão; não libera um payload anteriormente alocado. */
int message_init(Message *message);
/* Libera o payload pertencente à mensagem e zera seus campos; aceita ponteiro nulo. */
void message_free(Message *message);

/* Valida e escreve os 98 bytes do header campo a campo, sem transmitir padding da struct. */
int protocol_serialize_header(const Header *header, uint8_t *buffer, size_t buffer_size);
/* Lê os campos dos 98 bytes; a validação semântica do header deve ser feita separadamente. */
int protocol_deserialize_header(Header *header, const uint8_t *buffer, size_t buffer_size);
/* Calcula CRC32 de um buffer; zero também é retorno para ponteiro nulo com tamanho positivo. */
uint32_t protocol_calculate_crc32(const uint8_t *data, size_t size);
/* Exige versão conhecida, tipo permitido e payload de até 5 MiB; não verifica CRC. */
int protocol_validate_header(const Header *header);
/* Valida a mensagem, calcula CRC em uma cópia do header e envia header seguido pelo payload. */
int protocol_send_message(int sock, const Message *message);
/* Lê header e payload completos, valida limites e CRC e entrega o payload alocado ao chamador. */
int protocol_receive_message(int sock, Message *message);

#endif /* PROTOCOL_H */

```

</details>

### remote_error.h

Erro remoto em dois bytes: versão e categoria estável. EINVAL e EBADMSG convergem na categoria de mensagem inválida; decodificar não reproduz necessariamente o errno original.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef REMOTE_ERROR_H
#define REMOTE_ERROR_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
/* Códigos wire independentes dos valores de errno do sistema operacional. */
static inline void remote_error_encode(int error, uint8_t output[2])
{
    output[0] = 1U;
    switch (error)
    {
    case ENOENT: output[1] = 1U; break;
    case ENOTUNIQ: output[1] = 2U; break;
    case EEXIST: output[1] = 3U; break;
    case EBADMSG: case EINVAL: output[1] = 4U; break;
    case ENOTSUP: output[1] = 5U; break;
    case EACCES: output[1] = 6U; break;
    case ENODATA: output[1] = 7U; break;
    case EMSGSIZE: case EOVERFLOW: output[1] = 8U; break;
    default: output[1] = 9U; break;
    }
}
static inline int remote_error_decode(const uint8_t *data, size_t size)
{
    if (data == NULL || size != 2U || data[0] != 1U) return EREMOTEIO;
    switch (data[1])
    {
    case 1U: return ENOENT;
    case 2U: return ENOTUNIQ;
    case 3U: return EEXIST;
    case 4U: return EBADMSG;
    case 5U: return ENOTSUP;
    case 6U: return EACCES;
    case 7U: return ENODATA;
    case 8U: return EMSGSIZE;
    default: return EIO;
    }
}
#endif

```

</details>

### rpc.h

Declara a chamada TCP com origem/destino e codecs compartilhados do JOIN.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef RPC_H
#define RPC_H

#include "node.h"
#include "protocol.h"

int rpc_call(const char *host, uint16_t port, const NodeID *source, const NodeID *destination, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response);
int rpc_decode_join_payload(const uint8_t *payload, size_t payload_size, NodeConfig *config);
int rpc_encode_join_payload(const NodeConfig *config, uint8_t output[JOIN_PAYLOAD_WIRE_SIZE]);

#endif

```

</details>

### storage.h

Expõe o catálogo local e cópias de documentos/chunks. A destruição libera memória sem apagar os dados persistentes.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef STORAGE_H
#define STORAGE_H

#include "transfer_types.h"

#include <stddef.h>

typedef struct Storage Storage;

int storage_create(const char *root, const NodeID *owner, Storage **output);
int storage_descriptors(Storage *storage, const ObjectID *id, MetadataChunk **output);
void storage_destroy(Storage *storage);
int storage_begin(Storage *storage, const TransferDocument *document);
int storage_put_chunk(Storage *storage, const TransferChunk *chunk);
int storage_commit(Storage *storage, const ObjectID *id, TransferDocument *document);
int storage_find(Storage *storage, TransferSelectorType type, const ObjectID *id, const char *name, TransferDocument *document);
int storage_read_chunk(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data);
int storage_list(Storage *storage, TransferDocument **documents, size_t *count);

#endif

```

</details>

### superpeer.h

Expõe identidade/configuração, membros e resultados de registro. SuperPeer permanece opaco para não expor a organização do vetor e seu mutex.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef SUPERPEER_H
#define SUPERPEER_H

#include "node.h"

#include <stddef.h>
#include <time.h>

#define SUPERPEER_DEFAULT_MEMBER_CAPACITY 16U

typedef enum
{
    SUPERPEER_MEMBER_ALIVE = 0
} SuperPeerMemberState;

typedef struct
{
    Node node;
    SuperPeerMemberState state;
    time_t last_seen;
} SuperPeerMember;

typedef struct
{
    NodeConfig node;
    size_t initial_member_capacity;
} SuperPeerConfig;

typedef struct SuperPeer SuperPeer;

typedef enum
{
    SUPERPEER_REGISTER_ERROR = -1,
    SUPERPEER_MEMBER_UPDATED = 0,
    SUPERPEER_MEMBER_ADDED = 1
} SuperPeerRegistrationResult;

/* Initializes a Super Peer configuration with a generated UUID. */
int superpeer_config_init(SuperPeerConfig *config, const char *ip, uint16_t port);

/* Initializes a Super Peer configuration with a caller-provided UUID. */
int superpeer_config_init_with_uuid(SuperPeerConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE]);

/* Creates a Super Peer and registers its local node in its member table. */
int superpeer_create(const SuperPeerConfig *config, SuperPeer **output);

/*
 * Releases the Super Peer and its member table. Callers must stop and join
 * worker threads that use the object before calling this function.
 */
void superpeer_destroy(SuperPeer *superpeer);

/* Copies the local Super Peer node into output. */
int superpeer_get_node(const SuperPeer *superpeer, Node *output);

/* Adds a member or refreshes the existing member with the same NodeID. */
SuperPeerRegistrationResult superpeer_register_node(SuperPeer *superpeer, const Node *node);

/* Removes a non-local member identified by its NodeID. */
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id);

/* Copies a registered member into output. */
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output);

/* Returns the current number of registered members. */
size_t superpeer_member_count(const SuperPeer *superpeer);

/* Returns one when node_id is registered and zero otherwise. */
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id);

#endif

```

</details>

### transfer_protocol.h

Operações internas de STORE/DOWNLOAD, codecs e IDs de transação. Tipos de domínio vêm de transfer_types.h.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef TRANSFER_PROTOCOL_H
#define TRANSFER_PROTOCOL_H

#include "compression.h"
#include "metadata.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    TRANSFER_STORE_BEGIN = 1,
    TRANSFER_STORE_CHUNK = 2,
    TRANSFER_STORE_COMMIT = 3,
    TRANSFER_STORE_ANNOUNCE = 5 /* formato C2 v2; código 4 antigo não é aceito */
} TransferStoreOperation;

typedef enum
{
    TRANSFER_DOWNLOAD_METADATA = 3 /* metadados com descritores, v2 */,
    TRANSFER_DOWNLOAD_CHUNK = 2
} TransferDownloadOperation;

#include "transfer_types.h"

int transfer_encode_announcement(const TransferDocument *document, const MetadataChunk *chunks, uint8_t **output, uint32_t *size);
int transfer_decode_announcement(const uint8_t *payload, size_t size, TransferDocument *document, MetadataChunk **chunks);
void transfer_fill_transaction_id(uint8_t output[TRANSACTION_ID_SIZE], const uint8_t source_node[NODE_ID_SIZE]);
int transfer_encode_document(const TransferDocument *document, uint8_t **output, uint32_t *output_size, uint8_t operation);
int transfer_decode_document(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document);
int transfer_encode_chunk(const TransferChunk *chunk, uint8_t operation, uint8_t **output, uint32_t *output_size);
int transfer_decode_chunk(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferChunk *chunk);
int transfer_encode_object_operation(uint8_t operation, const ObjectID *id, uint8_t **output, uint32_t *output_size);
int transfer_decode_object_operation(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, ObjectID *id);
int transfer_encode_lookup_request(const char *selector, uint8_t **output, uint32_t *output_size);
int transfer_decode_lookup_request(const uint8_t *payload, size_t payload_size, TransferSelectorType *type, ObjectID *id, char name[METADATA_NAME_SIZE]);
int transfer_encode_lookup_result(const TransferLookupResult *result, uint8_t **output, uint32_t *output_size);
int transfer_decode_lookup_result(const uint8_t *payload, size_t payload_size, TransferLookupResult *result);
void transfer_lookup_result_free(TransferLookupResult *result);
int transfer_encode_chunk_request(const ObjectID *id, uint64_t index, uint8_t **output, uint32_t *output_size);
int transfer_decode_chunk_request(const uint8_t *payload, size_t payload_size, ObjectID *id, uint64_t *index);

#endif

```

</details>

### transfer_types.h

Separa tipos de domínio do encoding wire. TransferDocument contém metadados; TransferChunk aponta para dados; TransferEndpoint contém NodeID/IP/porta; TransferLookupResult possui vetores alocados por chunk. REPLICATED é estado reservado, não prova de replicação.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef TRANSFER_TYPES_H
#define TRANSFER_TYPES_H
#include "compression.h"
#include "metadata.h"
typedef enum
{
    TRANSFER_SELECTOR_OBJECT_ID = 1,
    TRANSFER_SELECTOR_NAME = 2
} TransferSelectorType;

typedef enum
{
    TRANSFER_CREATED = 0,
    TRANSFER_QUEUED = 1,
    TRANSFER_STARTED = 2,
    TRANSFER_TRANSFERRING = 3,
    TRANSFER_VERIFYING = 4,
    TRANSFER_FINISHED = 5,
    TRANSFER_REPLICATED = 6
} TransferState;

typedef struct
{
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    uint64_t file_size;
    uint64_t chunk_count;
    uint8_t compression;
} TransferDocument;

typedef struct
{
    ObjectID id;
    uint64_t index;
    uint64_t offset;
    uint32_t raw_size;
    uint32_t compressed_size;
    uint8_t hash[OBJECT_ID_SIZE];
    const uint8_t *data;
} TransferChunk;

typedef struct
{
    NodeID node_id;
    char ip[NODE_ADDRESS_SIZE];
    uint16_t port;
} TransferEndpoint;

typedef struct
{
    MetadataChunk descriptor;
    size_t peer_count;
    TransferEndpoint *peers;
} TransferChunkLocations;

typedef struct
{
    TransferDocument document;
    TransferChunkLocations *chunks;
} TransferLookupResult;
#endif

```

</details>

### wire.h

Helpers inline de inteiros em big-endian, sem dependência do layout de structs.

<details>
<summary>Declarações e tipos completos</summary>

```c
#ifndef WIRE_H
#define WIRE_H
#include <stdint.h>
#include <stddef.h>
static inline void wire_put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static inline uint16_t wire_get_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

static inline void wire_put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static inline uint32_t wire_get_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) | ((uint32_t)source[2] << 8) | source[3];
}

static inline void wire_put_u64(uint8_t *destination, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        destination[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static inline uint64_t wire_get_u64(const uint8_t *source)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        value = (value << 8) | source[index];
    }
    return value;
}

#endif

```

</details>

### Makefile e tests/c1/Makefile

O Makefile liga conjuntos explícitos de fontes: peer.c e superpeer.c não entram no mesmo executável. all cria os dois programas e os aliases. test-aluno2 exercita APIs locais; test-c2 executa testes locais negativos/atômicos; a integração completa entre processos está nos scripts, não é automaticamente equivalente a make test-c2. CPPFLAGS trata includes, CFLAGS compilação e LDFLAGS/LDLIBS ligação. BIN_DIR permite saídas isoladas. deps obtém headers oficiais dos pacotes Debian/Ubuntu sem instalar manualmente declarações de ABI.

<details>
<summary>Makefile: receitas completas</summary>

```makefile
CC ?= gcc
CFLAGS ?= -std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
CRYPTO_LIB ?= $(shell pkg-config --libs libcrypto 2>/dev/null || echo -Wl,-l:libcrypto.so.3)
LZ4_LIB ?= $(shell pkg-config --libs liblz4 2>/dev/null || echo -Wl,-l:liblz4.so.1)
LDLIBS ?= -pthread $(CRYPTO_LIB) -lz

BIN_DIR ?= bin
CPPFLAGS ?=
ifneq ($(wildcard .deps/usr/include/openssl/evp.h),)
CPPFLAGS += -I$(CURDIR)/.deps/usr/include -I$(CURDIR)/.deps/usr/include/x86_64-linux-gnu
endif
LDFLAGS ?=

.PHONY: all clean test test-aluno2 test-c2 deps

# Headers oficiais locais, sem sudo; somente para Debian/Ubuntu sem pacotes -dev.
deps:
	mkdir -p .deps/packages
	cd .deps/packages && apt-get download libssl-dev liblz4-dev
	cd .deps/packages && dpkg-deb -x ./libssl-dev_*.deb .. && dpkg-deb -x ./liblz4-dev_*.deb ..

all: $(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client

$(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer $(BIN_DIR)/test_transfer_negative $(BIN_DIR)/test_storage_atomic: $(wildcard *.h)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)


$(BIN_DIR)/superpeer: app_config.c app_config.h transfer_types.h remote_error.h network.c network.h concurrent_server.c concurrent_server.h protocol.c protocol.h transfer_protocol.c transfer_protocol.h \
                      node.c node.h common.h metadata.c metadata.h directory.c directory.h \
                      superpeer.c membership.c superpeer.h rpc.c rpc.h superpeer_app.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread app_config.c network.c concurrent_server.c protocol.c node.c \
		transfer_protocol.c rpc.c metadata.c directory.c membership.c superpeer.c superpeer_app.c \
		-o $@ $(LDLIBS)

$(BIN_DIR)/peer: app_config.c app_config.h transfer_types.h remote_error.h network.c network.h concurrent_server.c concurrent_server.h protocol.c protocol.h transfer_protocol.c transfer_protocol.h \
                 compression.c compression.h content.c content.h storage.c storage.h rpc.c rpc.h \
                 node.c node.h metadata.c metadata.h peer_service.c peer_service.h \
                 file_client.c file_client.h local_control.c local_control.h peer.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. app_config.c network.c concurrent_server.c protocol.c transfer_protocol.c compression.c content.c \
		storage.c rpc.c node.c metadata.c peer_service.c file_client.c local_control.c peer.c \
		-o $@ $(LDLIBS) $(LZ4_LIB)

$(BIN_DIR)/node: $(BIN_DIR)/superpeer Makefile | $(BIN_DIR)
	ln -sf superpeer $@

$(BIN_DIR)/client: $(BIN_DIR)/peer Makefile | $(BIN_DIR)
	ln -sf peer $@

test: all test-aluno2
	$(MAKE) -C tests/c1
	./tests/c1/test_protocol
	$(MAKE) test-c2

clean:
	rm -f $(BIN_DIR)/peer $(BIN_DIR)/superpeer $(BIN_DIR)/node $(BIN_DIR)/client $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer $(BIN_DIR)/test_transfer_negative
	$(MAKE) -C tests/c1 clean

# API local do aluno 2: não depende de sockets nem altera executáveis versionados.
$(BIN_DIR)/test_metadata: metadata.c metadata.h node.h common.h tests/c2/test_metadata.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. metadata.c tests/c2/test_metadata.c -o $@ $(CRYPTO_LIB)

$(BIN_DIR)/test_node_superpeer: node.c node.h membership.c superpeer.h common.h test_node_superpeer.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. node.c membership.c test_node_superpeer.c -o $@ $(CRYPTO_LIB)

test-aluno2: $(BIN_DIR)/test_metadata $(BIN_DIR)/test_node_superpeer
	$(BIN_DIR)/test_node_superpeer
	$(BIN_DIR)/test_metadata

$(BIN_DIR)/test_transfer_negative: network.c protocol.c transfer_protocol.c compression.c content.c storage.c metadata.c tests/c2/test_transfer_negative.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. network.c protocol.c transfer_protocol.c compression.c content.c storage.c metadata.c tests/c2/test_transfer_negative.c -o $@ $(LDLIBS) $(LZ4_LIB)

$(BIN_DIR)/test_storage_atomic: storage.c content.c compression.c metadata.c tests/c2/test_storage_atomic.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -I. storage.c content.c compression.c metadata.c tests/c2/test_storage_atomic.c -o $@ $(LDLIBS) $(LZ4_LIB)

test-c2: $(BIN_DIR)/test_transfer_negative $(BIN_DIR)/test_storage_atomic
	$(BIN_DIR)/test_transfer_negative
	$(BIN_DIR)/test_storage_atomic

```

</details>

<details>
<summary>tests/c1/Makefile: receitas completas</summary>

```makefile
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
LDLIBS ?= -pthread -lz

.PHONY: all clean

all: test_protocol

test_protocol: test_protocol.c ../../network.c ../../network.h \
               ../../protocol.c ../../protocol.h ../../common.h
	$(CC) $(CFLAGS) -pthread -I../.. ../../network.c ../../protocol.c \
		test_protocol.c -o $@ $(LDLIBS)

clean:
	rm -f test_protocol

```

</details>


## 29. Diagnóstico prático dos erros relatados durante esta revisão

### C1: respostas corretas, mas log vazio

A inspeção encontrou um Super Peer já escutando em 55101 e o arquivo de log do script com zero bytes. O segundo servidor não conseguiu abrir a porta; seus PINGs acabaram atendidos pelo processo antigo. Isso explica testes de rede aprovados e verificações de inicialização/RX PING reprovadas no log novo. Encerrar suas instâncias manuais antes da suíte ou escolher outra porta livre evita misturar os processos. O código de startup atual não imprime diagnóstico nesse caminho de falha, o que dificulta perceber o conflito.

### C2: recebido.pdf sobe, mas não é encontrado pelo nome

A leitura dos arquivos confirmou o mesmo SHA-256 para recebido.pdf e documento_a.pdf:

```text
40a4fb67dd42d916fb07dee35e327b56adfe390bcc22419fe30de5cd42e18e91
```

O manifest já registra documento_a.pdf. storage_begin aceita a repetição do mesmo ID/tamanho sem cadastrar outro nome; metadata_announce preserva o nome anterior. O runner enumera os dois arquivos e tenta baixar recebido.pdf pelo nome novo. A consulta encontra ausência, embora os bytes do objeto existam. Reiniciar não altera isso, pois os manifests são recarregados.

Baixar pelo ObjectID é a operação suportada para localizar esses mesmos bytes independentemente do nome. Suportar os dois nomes exigiria aliases de metadados ou outra decisão explícita de modelo; ajustar o teste para usar ObjectID também mudaria o que ele verifica. Este guia apenas explica a situação: não alterou os testes, não apagou PDFs e não mudou o modelo para mascarar a falha.

## 30. Exercícios para estudar sem decorar o código

1. Siga um upload: CLI → socket Unix → FileSession → BEGIN → workers → CHUNK → COMMIT → ANNOUNCE. Em cada seta, identifique processo, função e proprietário dos buffers.
2. Pegue um arquivo de 4 MiB + 1 byte. Calcule índices, offsets e tamanhos dos dois chunks. Explique por que o segundo tem apenas um byte original.
3. Mostre onde um erro de CRC impede chegar ao handler e onde um SHA de chunk inválido é rejeitado depois do framing.
4. Localize todos os recursos obtidos por file_client_download e o caminho que os libera se o segundo endpoint também falhar.
5. Explique por que nomes diferentes podem produzir o mesmo ObjectID e por que um NodeID não deve ser substituído pelo IP do socket.
6. Diferencie o que o teste de metadata prova sem rede do que integration.py verifica com processos reais.
7. Encontre onde FINISHED se torna visível em memória e o que já foi sincronizado no disco nesse momento.
8. Explique por que o Super Peer reiniciar sozinho não provoca reanúncio automático pelos Peers já ativos nesta implementação.

## 31. Cobertura e manutenção deste guia

Foram catalogadas 234 definições C, incluindo static e inline, mais 15 funções Python e 25 funções Bash. Os headers estão incluídos como contratos e os Makefiles como regras, sem contá-los como funções C extras. Os corpos principais dos scripts também foram documentados.

O escopo é o código próprio de produção e teste inventariado no projeto. PDFs, arquivos de dados, manifests gerados, binários, logs e arquivos Markdown não contêm funções implementadas a catalogar. Bibliotecas externas e headers de terceiros em .deps não são código autoral do trabalho e não tiveram suas funções internas reproduzidas.

O código expansível é uma fotografia desta revisão. Após alterar uma função, atualize a explicação e o trecho correspondente; as linhas de referência podem mudar. O catálogo de chamadas foi levantado estaticamente, não por instrumentação em runtime. Nenhuma suíte foi reexecutada para produzir este documento: exemplos e diagnóstico não substituem evidências de um novo teste.
