# Super Peer como servidor de clientes — design C5

## Intenção

Fazer o fluxo de cliente esperado por `tests/c5/run.sh` funcionar sem alterar esse runner nem `tests/config/c5.conf`. Os clientes devem poder enviar ANNOUNCE por upload, consultar documentos por nome, e os Super Peers devem armazenar e servir seus chunks com cache LFU. A replicação dos metadados continua usando o 2PC e a recuperação de catálogo continua usando IST.

## Estado atual

`superpeer.c` é o servidor TCP de associação, consulta de metadados e coordenação C4/C5. `peer.c` mantém a API de armazenamento: `Storage`, recepção de M_STORE (begin/chunk/commit), atendimento de M_DOWNLOAD_REQ e cache LFU. O cliente `bin/client` é um link para `bin/peer`; `--cmd upload` envia um comando à API local de controle de um Peer servidor, que precisa estar ativo. O runner inicia três Super Peers, mas nenhum Peer servidor. Suas buscas usam `--name`, enquanto a CLI atual de lookup aceita apenas ObjectID.

## Abordagens consideradas

1. **Super Peer armazena e serve os dados (escolhida pelo usuário).** Reutiliza a API `Storage` e o cache LFU existentes dentro do serviço Super Peer. O Super Peer recebe uploads e downloads diretamente, enquanto ANNOUNCE continua sendo replicado pelo 2PC. Isso é compatível com o runner sem processos Peer adicionais, mas amplia a responsabilidade do Super Peer.
2. **Super Peer como gateway para Peers separados.** Mantém a divisão atual entre metadados e dados, mas o runner não inicia Peers. Não atende ao requisito de executar o script sem alterações.
3. **Iniciar Peers no runner.** É a menor alteração de arquitetura, mas altera o arquivo que o usuário pediu para manter intacto.

## Contrato proposto

- O Super Peer recebe operações `M_STORE` de cliente para início, chunks e confirmação; chunks confirmados são persistidos pelo módulo `Storage` sob um diretório por porta/identidade do Super Peer.
- Após o armazenamento durável do documento, o Super Peer anuncia metadados/localização por `c4_cluster_submit`; somente o caminho COMMIT aplica o ANNOUNCE ao catálogo distribuído.
- O Super Peer responde `M_LOOKUP` por ObjectID ou nome e pode servir `M_DOWNLOAD_REQ` a partir da própria `Storage`; leituras de chunks passam pela LFU já existente.
- A CLI aceita `lookup --name` além de `--object-id`; os clientes usam o endereço do Super Peer como endpoint para upload, lookup e download.
- A cache é exercitada em requisições de download. Como o runner original emite duas buscas de metadados, o usuário aprovou que `lookup --name` leia o primeiro chunk válido do documento e descarte os bytes, aquecendo a cache por uma leitura real sem baixar o documento completo.
- O handler de IST registra eventos legíveis somente quando envia/aplica lotes reais; não se imprimem evidências artificiais para satisfazer `grep`.

## Fora de escopo

- Alterar `tests/c5/run.sh` ou `tests/config/c5.conf`.
- Remover armazenamento independente de Peer para topologias que ainda o utilizem.
- Alterar o wire format C4LG, a decisão 2PC, ou reduzir validação de CRC/índice/versão.
- Alegar que 2PC fornece consenso sob partições de rede.

## Riscos e validação necessária

A aceitação de upload diretamente pelo Super Peer precisa separar payloads de chunk de payloads ANNOUNCE já encaminhados, validar remetente/destino e evitar que clientes não registrados assumam identidade de membro. Armazenamento por Super Peer pode consumir espaço duplicado quando vários Super Peers recebem cópias. A aplicação local de um ANNOUNCE deve continuar posterior a COMMIT.

Depois da aprovação do design e da decisão pendente sobre cache em lookup, a validação deve incluir build estrito, `bash tests/c5/run.sh` sem mudar o runner/configuração, testes locais de storage/cache/journal e a integração TCP existente. Também deve confirmar que Super Peers continuam aceitando os fluxos existentes de Peer.
