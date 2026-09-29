# Checkpoint 2 — contrato integrado dos dois alunos

A arquitetura, comandos, requisitos e evidências atuais estão em [refatoracao_checkpoint_2.md](refatoracao_checkpoint_2.md).

## API local e integração TCP são coisas diferentes

A API local de membros está em `membership.c`, com declarações em `superpeer.h`. `superpeer.c` contém somente main; `superpeer_app.c` integra o atendimento TCP. Os testes locais não substituem os testes de rede.

A API de metadados foi ampliada com autorização explícita. A antiga limitação de manter sua estrutura inalterada não vigora nesta revisão:

- `metadata_announce`: recebe documento, descritores completos e NodeID anunciante; aloca a substituição antes de publicar, sob mutex. Retorna -1/errno sem alterar o cadastro em conflito ou falha.
- `metadata_find_name`: resolve nome no mesmo índice, ENOENT se ausente e ENOTUNIQ se ambíguo.
- `metadata_chunk_descriptor`: devolve cópia de descritor/hash por índice.
- `metadata_remove_peer`: remove disponibilidades do NodeID sem apagar documentos.
- APIs legadas de documento/chunk esparso continuam disponíveis para testes e compatibilidade local; o ANNOUNCE de rede usa a API atômica.

`MetadataDocument` contém ObjectID, nome, uint64 tamanho/contagem/versão, owner NodeID completo e compressão. `MetadataChunk` contém índice/offset uint64, tamanhos uint32 e hash de 32 bytes. Versão inicial é 1; não há algoritmo de versionamento distribuído.

## Fluxo entre componentes

1. Peer prepara listener e identidade persistente.
2. JOIN envia descritor; SP recalcula e valida NodeID.
3. Peer aprende e valida a identidade do SP na resposta.
4. COMMIT local verifica todos os chunks e ObjectID.
5. ANNOUNCE v2 envia documento e descritores; SP valida membership e publica o índice atômico.
6. Outro Peer ativo envia LOOKUP; SP devolve descritores e localizações NodeID/IP/porta.
7. DOWNLOAD_REQ/REP trafegam diretamente entre Peers. Destinos e respostas são correlacionados.
8. LEAVE voluntário remove membro e suas disponibilidades.

Origem zero é rejeitada no C2; somente comandos legados de diagnóstico C1 continuam admitindo anonimato. Isso não fornece autenticação criptográfica: o trabalho não implementa TLS ou credenciais federadas.

## Memória, erros e compatibilidade

Inteiros wire são big-endian. Structs e ponteiros não são transmitidos. Encoders alocam buffers liberados pelo chamador; consultas de peers devolvem cópias liberadas por free. Lookup composto usa transfer_lookup_result_free. Descritor de chunk decodificado aponta para bytes da mensagem recebida.

Erros de domínio usam payload de dois bytes (versão 1 e código estável); nunca se transmite errno bruto. Formatos: [protocolo_checkpoint_2.md](protocolo_checkpoint_2.md).

O índice do SP continua volátil. Ao reiniciar SP e Peers, manifests são reanunciados. Somente reiniciar SP não solicita reanúncio automático de Peers que já estavam ativos. Fallback tenta localizações existentes, sem criar réplicas.

## Evidências

`make test-aluno2` cobre APIs locais; `make test-c2` cobre protocolo/storage e falha real de gravação de manifest. `tests/c2/integration.py` cobre a rede com dois Peers, configuração, origem, erros, fallback e reinício do índice. Resultados e limites estão no relatório da refatoração. C3+ permanece fora do escopo.
