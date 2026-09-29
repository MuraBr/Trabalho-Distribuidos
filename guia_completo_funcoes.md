# Guia completo das funções do projeto

Este guia descreve o código refatorado em 28–29/09/2026. O objetivo é servir como mapa de leitura, não substituir o código-fonte: cada entrada explica o motivo da função, o que recebe, o que devolve e suas consequências mais importantes. Abrange todos os arquivos C de produção, os testes C, os scripts de teste e os headers. Funções da biblioteca C/POSIX, OpenSSL, zlib e LZ4 são chamadas pelo projeto, mas não são implementadas nele.

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
| **concurrent_server.h** | Runtime de aceitação e uma thread por conexão; callback de atendimento. |
| **rpc.h** | Uma chamada TCP requisição–resposta e o payload de JOIN. |
| **peer_service.h** | Inicialização do Peer em modo servidor de armazenamento. |
| **file_client.h** | Upload, download e benchmark iniciados pelo usuário. |

O campo Header.source_node é um NodeID **declarado na mensagem**, não o IP observado pelo socket. O log ip_origem de superpeer_app.c vem de getpeername. Para clientes de LOOKUP, o source_node pode estar zerado porque rpc_call foi chamado sem um NodeID.

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
- **option_command(argc, argv)**: interpreta pares de opções --cmd, --host, --port, --file e --output. Para upload/download chama file_client; para ping/join/leave chama legacy_command. Exige host e porta explícitos na sintaxe com --cmd.
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

**redvidassobretrabalhodepd/run.sh** importa common.sh, extrai fixtures quando necessário, escolhe arquivos .pdf sem exigir assinatura, inicia SP e Peer, seleciona o executor local, faz upload/download e compara bytes. Os três PDFs sintéticos participam do teste. A versão previamente adaptada foi preservada em run.pre-refactor.sh.

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
