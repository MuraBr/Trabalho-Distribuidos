# Guia completo das funções do projeto

Este guia descreve o código presente em 28/09/2026. O objetivo é servir como mapa de leitura, não substituir o código-fonte: cada entrada explica o motivo da função, o que recebe, o que devolve e suas consequências mais importantes. Abrange todos os arquivos C de produção, os testes C, os scripts de teste e os headers. Funções da biblioteca C/POSIX, OpenSSL, zlib e LZ4 são chamadas pelo projeto, mas não são implementadas nele.

## 1. Antes das funções: como o programa está dividido

O Makefile produz dois programas reais: **bin/superpeer**, cujo main condicional está em **superpeer.c**, e **bin/peer**, cujo main está em **peer.c**. **bin/node** aponta para bin/superpeer e **bin/client** aponta para bin/peer. O antigo **client.c** e o pequeno **teste.c** não entram no build padrão. O atendimento TCP do Super Peer fica em **superpeer_app.c**, chamado pelo main de superpeer.c.

Fluxo resumido:

1. O Super Peer inicia sua identidade, tabela de membros e índice de metadados.
2. O Peer de armazenamento carrega seu UUID e manifests, faz JOIN no Super Peer e reanuncia objetos finalizados.
3. No upload, o cliente calcula o ObjectID, divide o PDF em chunks de 4 MiB, calcula hashes, comprime com LZ4 e envia STORE ao Peer.
4. O Peer verifica cada chunk, persiste em pending, valida o documento inteiro no COMMIT, publica em objects e anuncia ao Super Peer.
5. No download, o cliente faz LOOKUP no Super Peer, recebe localizações e busca cada chunk diretamente dos Peers. Descomprime, verifica hashes e publica o destino somente após conferir o ObjectID completo.

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

Headers declaram tipos, constantes e protótipos; **não contêm corpos de função** neste projeto. A implementação de cada protótipo está explicada na seção do .c correspondente.

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
| **concurrent_server.h** | Runtime de aceitação e uma thread por conexão; callback de atendimento. |
| **rpc.h** | Uma chamada TCP requisição–resposta e o payload de JOIN. |
| **peer_service.h** | Inicialização do Peer em modo servidor de armazenamento. |
| **file_client.h** | Upload, download e benchmark iniciados pelo usuário. |

O campo Header.source_node é um NodeID **declarado na mensagem**, não o IP observado pelo socket. O log ip_origem de superpeer_app.c vem de getpeername. Para clientes de LOOKUP, o source_node pode estar zerado porque rpc_call foi chamado sem um NodeID.

## 3. Rede básica — network.c

- **network_create_server(porta, backlog)**: cria socket IPv4/TCP, ativa SO_REUSEADDR, associa-o a INADDR_ANY:porta com bind e passa a escutar com listen. backlog é o tamanho solicitado para a fila de conexões pendentes, não o número máximo total de clientes. Retorna o descritor de escuta; em erro fecha o que tiver sido criado e retorna -1.
- **network_accept_client(server_fd)**: chama accept no socket de escuta e devolve um novo descritor, exclusivo da conexão aceita. O descritor do servidor continua separado. A função coleta sockaddr_in, mas não o devolve; o Super Peer usa getpeername mais tarde para registrar o IP.
- **network_connect(ip, porta)**: abre um socket IPv4, converte um IP **numérico** com inet_pton e conecta ao servidor remoto. Não resolve nomes DNS. Em falha fecha o socket e devolve -1.
- **network_send_all(sock, buffer, tam)**: repete send até transmitir todos os bytes, pois uma chamada isolada pode enviar apenas parte deles. Trata EINTR e usa MSG_NOSIGNAL para não derrubar o processo em conexão quebrada. Devolve a quantidade total ou -1.
- **network_recv_exact(sock, buffer, tam)**: repete recv até obter exatamente tam bytes. Se o remoto fecha antes, devolve a quantidade parcial, inclusive 0 quando não recebeu nada; em erro devolve -1. O protocolo considera um header/payload parcial inválido.
- **network_shutdown(sock)**: tenta shutdown nos dois sentidos e depois close. O retorno reflete close; portanto uma mensagem de erro de shutdown pode aparecer mesmo que close seja bem-sucedido.

## 4. Header, framing e CRC — protocol.c

O TCP entrega fluxo de bytes, não mensagens. O protocolo primeiro transmite 98 bytes de header. O campo payload_size informa quantos bytes ler em seguida. Os inteiros são serializados em big-endian e o checksum é CRC32 do header com checksum zerado, seguido do payload.

- **write_u32_be / read_u32_be**: escrevem/leem um uint32_t em quatro bytes de ordem fixa. São auxiliares privados, usados para tamanho e CRC do header.
- **write_u64_be / read_u64_be**: fazem o mesmo para uint64_t em oito bytes, usado no timestamp. A conversão explícita evita depender da ordem de bytes do processador ou do padding de struct.
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
- **put_u16 / get_u16, put_u32 / get_u32, put_u64 / get_u64**: gravam e leem números multibyte em big-endian nos payloads. São privados deste módulo; storage.c tem equivalentes próprios para o formato de disco.
- **object_id_from_hex(id, text)**: aceita exatamente 64 dígitos hexadecimais e converte para os 32 bytes do ObjectID. É usado para distinguir um seletor por ID de um nome de arquivo.
- **document_wire_size(document)**: calcula o espaço necessário para operação, campos fixos e nome sem NUL.
- **encode_document_at(document, operation, output, capacity, used)**: valida nome, LZ4, contagem esperada de chunks e espaço; escreve operação, ObjectID, tamanho, contagem, compressão e nome. Informa bytes efetivamente usados.
- **decode_document_at(payload, size, expected_operation, document, used)**: faz a leitura inversa com checagens de tamanho, operação, nome, compressão e contagem de chunks. Rejeita NUL ou barra dentro do nome transmitido.
- **transfer_fill_transaction_id(output, source_node)**: combina timestamp, quatro bytes iniciais do NodeID (ou zeros) e contador atômico de 32 bits. A resposta não cria novo ID: copia exatamente o da requisição.
- **transfer_encode_document / transfer_decode_document**: interface pública para documentos usados em STORE/BEGIN, STORE/ANNOUNCE e metadados de download. O decoder público exige consumir o payload inteiro, impedindo bytes extras ocultos.
- **transfer_encode_chunk / transfer_decode_chunk**: codificam e decodificam operação, ObjectID, índice, offset, tamanhos, SHA-256 do conteúdo original e bytes LZ4. Validam comprimentos básicos e limite de 4 MiB do dado original; a verificação criptográfica ocorre em storage.c ou file_client.c.
- **transfer_encode_object_operation / transfer_decode_object_operation**: formato curto de operação + ObjectID, usado no STORE/COMMIT.
- **transfer_encode_lookup_request(selector, ...)**: se o texto for um ObjectID hexadecimal válido, envia seletor binário por ID; caso contrário, envia nome. Aloca o payload.
- **transfer_decode_lookup_request(payload, ...)**: lê tipo e comprimento do seletor; valida comprimento, ausência de barra/NUL no nome e separa ObjectID ou nome.
- **transfer_lookup_result_free(result)**: libera listas de endpoints de cada chunk e o vetor de chunks; zera a estrutura. Deve ser chamado para um resultado de lookup montado ou decodificado.
- **transfer_encode_lookup_result(result, ...)**: serializa documento e, para cada chunk, quantidade de Peers e seus NodeIDs, IPs e portas. Calcula tamanho e rejeita excesso antes de alocar.
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
- **content_validate_pdf(path)**: exige extensão .pdf (sem diferenciar maiúsculas) e procura a assinatura %PDF- nos primeiros 1024 bytes. É uma verificação superficial, **não** um parser completo de PDF. Os documento_*.pdf do pacote de teste fornecido não passam nessa regra.
- **content_basename(path, output)**: extrai o último componente após / e valida se cabe em METADATA_NAME_SIZE. Esse nome é metadado; caminhos físicos internos usam ObjectID, não o nome recebido.

## 7. Identidade dos nós — node.c

- **normalize_ip(ip, normalized)**: auxiliar privado que tenta interpretar IPv4 e depois IPv6 por inet_pton; reescreve a forma textual normalizada com inet_ntop. NodeID depende dos bytes binários, não de diferenças de grafia do endereço.
- **node_generate_uuid(uuid)**: lê 16 bytes de /dev/urandom, repetindo leituras parciais e tratando EINTR. Ajusta os bits de versão/variante de UUID v4. Um UUID novo muda o NodeID mesmo com IP e porta iguais.
- **node_config_validate(config)**: exige porta diferente de zero, IP terminado dentro do buffer e endereço interpretável. Não testa conectividade.
- **node_config_init_with_uuid(config, ip, port, uuid)**: normaliza IP e preenche configuração usando UUID recebido. É essencial para reconstruir, no receptor, a mesma identidade anunciada no JOIN.
- **node_config_init(config, ip, port)**: gera UUID novo e chama a variante anterior. O próprio node.c não persiste UUID; peer_service.c faz essa persistência para Peers de armazenamento.
- **node_compute_id(config, id)**: calcula SHA-256 dos bytes binários do IP, porta em ordem de rede e UUID. PID e papel não entram no hash. Usa libcrypto.
- **node_init(node, config)**: calcula NodeID, copia configuração, registra getpid local e define papel inicial PEER. O PID reconstruído no servidor é local ao servidor, não o PID remoto.
- **node_validate(node)**: recalcula o NodeID, compara-o ao armazenado e verifica PID positivo e papel permitido. Verifica consistência interna da estrutura, não autentica uma máquina externa.
- **node_id_to_hex(id, output, output_size)**: escreve os 32 bytes como 64 dígitos hexadecimais mais NUL; exige buffer suficiente.
- **hexadecimal_value(character)**: auxiliar privado que converte um único dígito hexadecimal, aceitando letras maiúsculas/minúsculas.
- **node_id_from_hex(id, hex)**: exige 64 dígitos e reconstrói 32 bytes. Em erro a saída pode estar parcialmente preenchida; não a trate como ID válido.
- **node_id_compare(left, right)**: comparação lexicográfica normalizada em -1, 0 ou 1; também define ordem quando ponteiros são nulos. É um comparador, não uma eleição Bully.
- **node_id_equal(left, right)**: compara os 32 bytes; ponteiros nulos não contam como IDs iguais.
- **node_get_process_id(node)**: devolve o PID armazenado, ou -1 para ponteiro nulo.

## 8. Tabela de membros e entrada do Super Peer — superpeer.c

SuperPeer é opaco fora do .c. Internamente contém nó local, vetor dinâmico de membros, contagem/capacidade e mutex. O primeiro membro é o próprio Super Peer. Esta tabela de membership **não** é o índice de documentos de metadata.c.

- **main(argc, argv)**: existe somente quando o Makefile define SUPERPEER_EXECUTABLE. Encaminha a execução a superpeer_run em superpeer_app.c. Sem essa definição, o mesmo superpeer.c pode ser ligado ao teste da API local sem duplicar main.
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
- **superpeer_unregister_node(superpeer, node_id)**: remove membro não local, compactando o vetor com memmove. Rejeita a remoção do próprio Super Peer. A função existe, mas o handler atual de LEAVE só responde ACK e não a chama.
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
- **free_entry(entry)**: libera a lista de disponibilidades e a entrada do documento.
- **metadata_create(output)**: aloca store zerado, inicializa mutex e devolve ponteiro opaco.
- **metadata_destroy(store)**: libera todas as entradas e o mutex; exige que outras threads já tenham parado de usá-lo.
- **metadata_register_document(store, id, name, file_size)**: cria documento e calcula chunk_count, ou aceita repetição do mesmo ID/tamanho. Se outro nome vier para o mesmo ID, o primeiro nome fica preservado; tamanho diferente gera EEXIST.
- **metadata_find_document(store, id, output)**: procura pelo ObjectID e devolve cópia de MetadataDocument. ENOENT quando não existe.
- **metadata_remove_document(store, id)**: remove documento e todas as suas associações de chunk; é uma operação da API local, não um fluxo de exclusão de documento distribuído.
- **change_chunk(store, id, index, peer, remove)**: auxiliar comum que valida documento/índice, busca a associação específica e insere ou remove sob o mesmo mutex. Inserção repetida é idempotente; remoção ausente gera ENOENT.
- **metadata_register_chunk / metadata_unregister_chunk**: wrappers que chamam change_chunk com modo inserir/remover. Não verificam se o NodeID está registrado em membership; essa responsabilidade pertence à integração.
- **metadata_chunk_peers(store, id, chunk_index, output, count)**: conta e copia todos os NodeIDs que anunciam aquele chunk. O chamador libera a matriz retornada com free; sem localizações, devolve NULL e contagem zero.

## 10. Ponte entre índice e membros — directory.c

Directory guarda ponteiros para MetadataStore e SuperPeer; **não é dono deles**. Mantém ainda uma lista nome→ObjectID protegida por mutex, porque a API atual de metadados busca por ID.

- **directory_create(metadata, superpeer, output)**: aloca Directory e mutex, guardando referências aos dois serviços existentes.
- **directory_destroy(directory)**: libera a lista local de nomes e o próprio Directory; não destrói metadata nem SuperPeer.
- **directory_announce(directory, document, owner)**: exige LZ4, registra documento e todos os índices de chunk para o NodeID dono, depois registra nome/ID na lista auxiliar. Anúncio repetido é aceito. Essa função registra **localização**, não armazena bytes do arquivo.
- **resolve_name(directory, name, id)**: procura na lista. Se o mesmo nome corresponder a dois ObjectIDs diferentes, devolve ENOTUNIQ para exigir seleção por ID; se não houver, ENOENT.
- **directory_lookup(directory, type, id, name, result)**: resolve o seletor, copia metadados, consulta NodeIDs de cada chunk e traduz cada um em IP/porta usando superpeer_find_member. Monta TransferLookupResult alocado; o chamador usa transfer_lookup_result_free. Falha se um chunk ficar sem localização utilizável.

## 11. Persistência local do Peer — storage.c

Storage administra .peer_storage/<porta>/pending e objects. Cada objeto tem pasta nomeada pelo ObjectID hexadecimal, um manifest.bin versionado e arquivos chunk-<índice>.lz4. O manifest armazena metadados, dono, timestamp, estado e descritores. O mutex protege o catálogo e operações de mudança. Nomes enviados por usuários **não** são usados para montar os caminhos físicos.

- **put_u16 / get_u16, put_u32 / get_u32, put_u64 / get_u64**: convertem escalares do manifest para/de big-endian. São necessários para que a representação persistida não dependa da arquitetura; não seriam necessários se o projeto escolhesse outro formato estável, mas gravar structs cruas seria frágil por endianness e padding.
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
- **storage_put_chunk(storage, chunk)**: descomprime temporariamente, calcula SHA-256 do original e compara com o hash anunciado; valida índice, offset e tamanho esperado. Grava bytes LZ4 em temporário, fsync/rename, atualiza descritor e manifest. Duplicata compatível é idempotente; incompatível gera EEXIST.
- **verify_document(storage, stored, final)**: reabre todos os chunks, descomprime/verifica cada hash, concatena o conteúdo em arquivo temporário e calcula ObjectID/tamanho da reconstrução. O arquivo de verificação é removido no sucesso ou erro.
- **storage_commit(storage, id, document)**: exige todos os chunks, chama verify_document, grava estado VERIFYING, renomeia a pasta de pending para objects e grava estado FINISHED. Devolve descritor para posterior ANNOUNCE. Repetição de objeto já finalizado é aceita.
- **storage_find(storage, type, id, name, document)**: consulta apenas documentos FINISHED por ID ou nome; nome ambíguo para IDs diferentes gera ENOTUNIQ.
- **storage_read_chunk(storage, id, index, chunk, owned_data)**: lê chunk comprimido de objeto finalizado e devolve descritor + buffer de bytes. O chamador é dono de owned_data e deve usar free; chunk.data aponta para o mesmo buffer.
- **storage_list(storage, documents, count)**: copia os documentos FINISHED do catálogo. O Peer usa a lista ao reiniciar para reanunciá-los; o chamador libera a matriz com free.

O manifest e o índice do Super Peer têm papéis diferentes: o primeiro permite ao Peer recuperar seus bytes locais; o segundo informa ao cliente **qual Peer** possui cada chunk. O Super Peer não guarda o conteúdo do PDF.

## 12. Servidor TCP concorrente — concurrent_server.c

- **remove_connection(connection)**: retira a conexão da lista protegida por mutex e sinaliza a condition variable. O runtime usa esse sinal para saber quando todos os atendimentos terminaram.
- **serve_connection(argument)**: função executada por uma thread. Chama o callback específico do Peer ou Super Peer, fecha o socket, descadastra a conexão e libera seu contexto.
- **concurrent_server_create(server_fd, handler, context, output)**: aloca runtime, inicializa mutex/condition variable e guarda o descritor de escuta e callback. O callback recebe context e o socket aceito.
- **concurrent_server_stop(server)**: marca parada sob mutex, fecha o socket de escuta e usa shutdown nas conexões ativas para desbloquear operações de leitura. Pode ser chamado mais de uma vez.
- **concurrent_server_run(server)**: laço de accept. Para cada conexão, cria um registro e uma thread destacada; trata EINTR e erros de criação. Ao sair, interrompe novas conexões e espera a lista de atendimentos esvaziar antes de retornar.
- **concurrent_server_destroy(server)**: solicita parada, destrói primitivas de sincronização e libera o runtime. O uso correto pressupõe que concurrent_server_run já concluiu e os callbacks terminaram.

## 13. Chamada TCP simples — rpc.c

- **rpc_encode_join_payload(config, output)**: valida NodeConfig e monta os 64 bytes do descritor JOIN: IP textual/padding, porta em ordem de rede e UUID. Essa representação não inclui PID nem envia a struct C crua.
- **rpc_call(host, port, source, destination, type, payload, payload_size, response)**: abre uma conexão nova, monta Message com tipo, IDs opcionais, TransactionID e timestamp; envia, recebe uma resposta e exige o mesmo TransactionID. Fecha o socket em todos os caminhos normais. Em sucesso, o chamador passa a ser dono do payload em response e deve chamar message_free.

## 14. Servidor de armazenamento — peer_service.c

PeerService combina Node, Storage, runtime concorrente e endereço do Super Peer. Uma conexão atendida pelo Peer processa uma requisição e devolve uma resposta; os workers de upload/download abrem suas próprias conexões.

- **stop_service(signal_number)**: handler de sinal que faz shutdown no socket de escuta para acordar o accept e iniciar a saída do serviço.
- **load_or_create_uuid(storage_path, uuid)**: cria .peer_storage/<porta>/node.uuid com 16 bytes e fsync, ou lê exatamente os 16 bytes existentes. Isso mantém o mesmo NodeID do Peer após reinicialização na mesma porta.
- **send_response(service, socket_fd, request, type, payload, payload_size)**: constrói resposta com source_node local, destination_node da requisição e **o mesmo TransactionID**. Copia o payload fornecido, envia pelo protocolo e libera a cópia.
- **join_superpeer(service)**: codifica o descritor de nó, envia M_JOIN via rpc_call ao Super Peer e só aceita M_ACK. Sem JOIN o serviço não prossegue.
- **announce_document(service, document)**: codifica STORE/ANNOUNCE e envia ao Super Peer com o NodeID do Peer; espera ACK. Não transfere bytes do PDF ao Super Peer.
- **announce_catalog(service)**: obtém storage_list e chama announce_document para cada objeto FINISHED. Reconstitui o índice volátil do Super Peer quando o Peer reinicia.
- **handle_store(service, socket_fd, message)**: despacha STORE/BEGIN para storage_begin, STORE/CHUNK para storage_put_chunk e STORE/COMMIT para storage_commit seguido de announce_document. Responde ACK apenas se a operação inteira deu certo; nos demais casos, ERROR.
- **handle_download(service, socket_fd, message)**: decodifica ObjectID/índice, lê chunk comprimido finalizado, codifica DOWNLOAD_REP e o envia. Libera payload codificado e o buffer que veio de storage_read_chunk.
- **serve_connection(context, socket_fd)**: callback da conexão aceita. Recebe uma Message e atende STORE, DOWNLOAD_REQ ou PING textual; qualquer outro tipo recebe ERROR. Libera a mensagem recebida.
- **initialize_service(service, local_port, superpeer_host, superpeer_port)**: monta caminho de armazenamento da porta, carrega/cria UUID, configura NodeID e chama storage_create para ler manifests locais.
- **peer_service_run(local_port, superpeer_host, superpeer_port)**: fluxo principal do modo serve: inicializa, faz JOIN, reanuncia catálogo, cria socket de escuta e entra no concurrent_server_run; na saída encerra runtime e Storage.

## 15. Cliente de arquivo — file_client.c

Há dois contextos de trabalho: UploadWork e DownloadWork. Ambos distribuem índices de chunks por mutex, guardam o primeiro erro e usam várias threads. Cada RPC abre conexão TCP própria. A quantidade de workers padrão é mínimo entre CPUs, chunks e 8; PEER_TRANSFER_THREADS aceita de 1 a 32.

- **transfer_worker_count(chunk_count)**: lê número de CPUs e eventual variável PEER_TRANSFER_THREADS; limita a quantidade ao número de chunks e impede zero workers.
- **request_expect(host, port, type, payload, payload_size, expected, response)**: chama rpc_call e exige o tipo de resposta indicado. Se vier ERROR ou outro tipo, libera a resposta e informa EREMOTEIO.
- **pread_all(fd, buffer, size, offset)**: lê um chunk inteiro em posição explícita do arquivo sem compartilhar o offset do descritor entre threads. Repete leituras parciais e EINTR.
- **pwrite_all(fd, buffer, size, offset)**: grava um chunk inteiro no offset correto do destino, também seguro contra interferência entre offsets de workers distintos. Repete escritas parciais e EINTR.
- **upload_fail(work, error)**: registra, sob mutex, somente o primeiro erro de upload. Workers posteriores param de pegar novos índices.
- **upload_worker(argument)**: pega próximo índice, lê bytes originais por pread_all, calcula SHA-256 do chunk, comprime com LZ4, codifica STORE/CHUNK e exige ACK do Peer. Atualiza total comprimido e hashes de saída; libera buffers temporários.
- **execute_upload_workers(work, worker_count)**: cria as threads de upload, aguarda todas com pthread_join e propaga o primeiro erro registrado. Mesmo após falha, não abandona threads ativas.
- **file_client_upload(path, peer_host, peer_port)**: valida assinatura/extensão PDF, calcula ObjectID incremental, define chunks de 4 MiB, envia STORE/BEGIN, executa workers e envia STORE/COMMIT. Só imprime Upload completed após o ACK do COMMIT, que por sua vez depende do ANNOUNCE ao Super Peer.
- **download_fail(work, error)**: equivalente de upload_fail para os workers de download.
- **download_one(work, index)**: pede o chunk aos endpoints anunciados, um por vez. Para cada resposta, confere descritor, offset/tamanho, descomprime LZ4, valida SHA-256 e usa pwrite_all; se um endpoint falha, tenta o próximo.
- **download_worker(argument)**: distribui índices aos workers até acabar a fila ou ocorrer o primeiro erro. Chama download_one para cada chunk obtido.
- **execute_download_workers(work, worker_count)**: cria/aguarda threads e devolve o primeiro erro da operação.
- **lookup_document(host, port, selector, result)**: codifica LOOKUP por nome ou ObjectID, consulta o Super Peer e decodifica documento + lista de Peers por chunk.
- **file_client_download(selector, destination, superpeer_host, superpeer_port)**: consulta LOOKUP, decide destino, rejeita sobrescrita, cria arquivo exclusivo com sufixo .part e usa workers para preencher offsets. Ao final faz fsync, recalcula ObjectID do arquivo inteiro, compara tamanho/hash e usa link para publicar sem substituir destino existente; remove o .part quando apropriado. Libera resultado de lookup.
- **file_client_benchmark_lz4(path)**: percorre um PDF em chunks e mede apenas a compressão LZ4; informa bytes, duração e taxa. Não é teste rígido de desempenho nem verifica rede.

## 16. Entrada do Peer e comandos — peer.c

- **parse_port(text, port)**: converte porta decimal válida de 1 a 65535, rejeitando texto extra.
- **message_name(type)**: devolve rótulo legível para os comandos legados PING/JOIN/LEAVE e respostas.
- **legacy_command(command, host, port)**: executa uma operação de compatibilidade do C1. PING envia quatro bytes e espera PONG; JOIN monta NodeID/descritor de teste e espera ACK; LEAVE espera ACK. Esse LEAVE não demonstra remoção real do membro.
- **option_command(argc, argv)**: interpreta pares de opções --cmd, --host, --port, --file e --output. Para upload/download chama file_client; para ping/join/leave chama legacy_command. Exige host e porta explícitos na sintaxe com --cmd.
- **usage(program)**: imprime todas as formas aceitas da linha de comando.
- **main(argc, argv)**: ponto de entrada de bin/peer e bin/client. Ignora SIGPIPE, escolhe serve, upload, download, benchmark ou comando legado; usa 127.0.0.1:55101 como Super Peer e 127.0.0.1:55102 como Peer de armazenamento quando o comando posicional omite endereço/porta. Retorna EXIT_FAILURE em erro.

## 17. Atendimento do Super Peer — superpeer_app.c

PeerContext neste arquivo é o estado do **processo Super Peer**, apesar do nome histórico. Ele reúne identidade local, membership, índice MetadataStore, Directory, socket e runtime concorrente.

- **handle_signal(signal_number)**: limpa a flag sig_atomic_t que mantém o processo vivo; a saída normal para o runtime é feita depois, fora do handler.
- **parse_port(text, port)**: valida porta decimal no intervalo permitido.
- **parse_command_type(text, type)**: converte ping, join ou leave para o código de mensagem correspondente.
- **parse_node_arguments(argc, argv, arguments)**: aceita forma posicional legada e opções getopt_long. Distingue modo servidor de modo --cmd e valida a combinação de argumentos; --config é verificado quanto à existência/leitura, não interpretado como configuração de porta.
- **print_node_id(node_id)**: imprime os 32 bytes como 64 dígitos hexadecimais.
- **message_type_name(type)**: devolve nome textual para logs TX/RX dos comandos C1.
- **set_text_payload(message, text)**: aloca e copia payload textual sem o NUL final; usado para PING. A mensagem passa a ser dona do buffer.
- **message_payload_equals(message, text)**: compara comprimento e bytes de um payload com texto esperado, sem exigir terminador NUL na rede.
- **node_id_is_zero(node_id)**: verifica se os 32 bytes são zero. JOIN inicial pode não conhecer o ID do destinatário e usar destino zerado.
- **encode_join_payload(config, payload, payload_size)**: escreve IP textual/padding, porta big-endian e UUID no descritor fixo de 64 bytes.
- **encode_join_payload_alloc(config, payload_output)**: aloca buffer JOIN, chama o encoder acima e transfere ownership ao chamador.
- **decode_join_payload(payload, payload_size, config)**: exige tamanho fixo, terminador NUL, padding zero e configuração de nó válida; reconstrói o NodeConfig sem transmitir uma struct crua.
- **initialize_local_identity(peer, local_port)**: cria Node local e SuperPeer membership com **o mesmo UUID**, garantindo que os dois objetos tenham o mesmo NodeID.
- **send_reply(peer, client_fd, request, type, payload, payload_size, include_node_descriptor)**: constrói resposta com NodeID local, destino da requisição, mesmo TransactionID e payload opcional; pode anexar o próprio descritor de nó em ACK de JOIN.
- **register_join(peer, message)**: valida destino, decodifica descritor, recalcula NodeID do remetente e compara ao header; rejeita autorregistro e inclui/atualiza membro antes do ACK.
- **register_announcement(peer, message)**: exige que source_node já esteja cadastrado, decodifica STORE/ANNOUNCE e delega registro de documento/chunks a directory_announce.
- **answer_lookup(peer, client_fd, message)**: decodifica seletor, consulta Directory, serializa metadados/localizações e responde DOWNLOAD_REP; em erro envia ERROR. Libera estruturas temporárias.
- **handle_client(context, client_fd)**: callback para conexão TCP. Usa getpeername para obter ip_origem real da conexão, recebe mensagens em laço, registra log e despacha JOIN, PING, LEAVE, ANNOUNCE e LOOKUP; mensagens futuras/inesperadas recebem ERROR. **origem** no log é o NodeID do header (exceto PING, em que é omitido), não o IP. LEAVE atualmente só recebe ACK.
- **accept_clients(argument)**: thread que chama concurrent_server_run no runtime do Super Peer; o runtime cria uma thread por conexão.
- **connect_and_join(peer, ip, remote_port)**: forma legada de conectar este servidor a outro nó, enviar JOIN e verificar ACK, TransactionID e identidade recebida; também registra o remoto em sua tabela local.
- **execute_command(arguments)**: modo --cmd de bin/superpeer/bin/node para PING, JOIN ou LEAVE. Monta mensagem, envia uma vez, confere resposta e encerra.
- **fill_transaction_id(transaction_id)**: forma legada de compor 16 bytes a partir de tempo, PID e contador local. É distinta da versão com contador atômico em transfer_protocol.c.
- **print_usage(program_name)**: mostra os modos aceitos pelo Super Peer, incluindo interface posicional legada.
- **superpeer_run(argc, argv)**: inicializa modo servidor ou executa comando único; no modo servidor cria identidade, MetadataStore, Directory e socket, inicia atendimento e aguarda sinal. No encerramento para conexões e libera recursos na ordem inversa. É chamado pelo main condicional de superpeer.c.

## 18. Arquivos C legados ou de demonstração

**client.c** é uma implementação antiga de cliente C1. O Makefile atual **não** o compila em bin/client; esse nome é link para bin/peer. Leia-o para entender a evolução do projeto, não como fonte do comportamento atual.

- **parse_port**: valida porta decimal.
- **parse_command**: traduz texto ping/join/leave para enum ClientCommand.
- **parse_arguments**: exige --cmd, --host e --port e rejeita argumentos não reconhecidos.
- **fill_transaction_id**: monta identificador de 16 bytes com tempo, PID e contador para correlacionar resposta.
- **encode_join_payload**: serializa o descritor de nó C1 com IP, porta e UUID.
- **print_usage**: mostra a sintaxe aceita por esse cliente antigo.
- **main**: conecta ao servidor, monta PING/JOIN/LEAVE, envia a mensagem, confere tipo e TransactionID da resposta e libera socket/memória. No JOIN usa uma porta de teste, sem abrir servidor naquela porta.

**teste.c** contém apenas **main**, que imprime o valor da macro de compilador __STDC_VERSION__ e retorna sucesso. É um experimento isolado do início do projeto; não participa do Makefile nem dos testes C1/C2.

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

- **fill_transaction_id**: preenche ID fixo e reproduzível para comparar ida e volta.
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
- **corpo principal**: compila teste de protocolo, inicia servidor, envia PING/JOIN/LEAVE, testa concorrência, framing, versão inválida e identificação. A aprovação C1 é 10/10.

**script_testes_peer.sh** — teste legado de bin/node atuando como nó em ambos os lados:

- **ok / notok**: imprimem/contam resultados.
- **cleanup**: encerra os processos iniciados por start_peer e os aguarda.
- **wait_for_log**: espera até certo limite uma expressão aparecer em log; evita depender apenas de sleep fixo.
- **start_peer**: inicia processo legado com saída direcionada ao log e guarda PID.
- **corpo principal**: PING concorrente, JOIN entre nós e validação de logs. O nome do script é histórico; ele usa o alias bin/node, não o servidor de chunks de peer_service.c.

**script_testes_c2.sh** — suíte principal do upload/download C2:

- **pass / fail**: registram resultados.
- **stop_pid / cleanup**: encerram Super Peer e Peers criados; cleanup também remove a pasta temporária específica da execução.
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

**redvidassobretrabalhodepd/run.sh** não define funções próprias: importa common.sh, extrai data.tar.gz quando necessário, escolhe apenas PDFs com assinatura, inicia Super Peer e Peer de armazenamento, faz upload/download de cada PDF escolhido, compara arquivos e verifica evidências no log. Os três documento_*.pdf do pacote são ignorados porque lhes falta a assinatura; o PDF real da raiz do projeto é usado. Esse script adaptado não cobre multichunk se o único PDF válido for pequeno.

**Makefile** não define funções C: suas regras ligam os módulos aos executáveis, criam aliases bin/node e bin/client, e expõem alvos como all, test, test-aluno2 e test-c2. Os .h são dependências para recompilação; a presença de um .c no diretório não significa que ele participa de um binário — confira a receita do alvo.

## 21. Roteiros de leitura para entender de fato

Para compreender uma requisição sem se perder, siga primeiro esta cadeia:

1. **Comando upload:** peer.c main → file_client_upload → rpc_call → network_connect → protocol_send_message.
2. **Recepção no Peer:** concurrent_server_run → peer_service.c serve_connection → handle_store → storage_begin/storage_put_chunk/storage_commit.
3. **Anúncio:** peer_service.c announce_document → rpc_call → superpeer_app.c handle_client → register_announcement → directory_announce → metadata_register_document/metadata_register_chunk.
4. **Download:** file_client_download → lookup_document → superpeer_app.c answer_lookup → directory_lookup → download_worker/download_one → peer_service.c handle_download → storage_read_chunk.
5. **Integridade:** CRC32 verifica a mensagem em trânsito; SHA-256 de cada chunk verifica seus bytes descomprimidos; o ObjectID verifica o documento inteiro. Essas três verificações não são intercambiáveis.

Ao estudar qualquer função, pergunte: **quem a chama?**, **quem é dono de cada buffer?**, **qual mutex protege o estado?**, **qual erro pode retornar?**, **em que ponto o dado passa a ser visível?** Esse conjunto de perguntas costuma revelar mais que ler linha por linha sem o fluxo em mente.

## 22. Limites reais da implementação

- Embora node.c aceite IPv6 para identidade, a camada network.c conecta/escuta apenas IPv4; não prometa operação IPv6 ponta a ponta.
- A verificação de PDF é assinatura/extensão, não validação semântica completa do formato.
- LEAVE/ACK não chama superpeer_unregister_node nem remove disponibilidade.
- MetadataStore do Super Peer é volátil; a recuperação depende de Peers reiniciarem e reanunciarem seus manifests.
- Não há autenticação criptográfica do remetente, TLS, DHT/Chord, Gossip, eleição, replicação automática, SMR, 2PC, LFU ou IST neste checkpoint. Um NodeID declarado e um IP observado são informações diferentes.
- Disponibilidade, consistência global e taxas de desempenho futuras não devem ser apresentadas como garantidas apenas porque a estrutura de código reserva nomes ou estados para elas.
