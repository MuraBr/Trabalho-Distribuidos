# Mapa atual dos arquivos C

A organização anterior foi substituída pela refatoração integrada C1/C2.

- [Arquitetura, módulos, configuração e comandos](refatoracao_checkpoint_2.md)
- [Explicação detalhada das funções](guia_completo_funcoes.md)
- [Contrato entre os alunos](integracao_checkpoint_2_aluno_2.md)
- [Wire e armazenamento](protocolo_checkpoint_2.md)

`peer.c` e `superpeer.c` são pontos de entrada exclusivos. `peer.c` também contém o serviço antes implementado em `peer_service.c`; `superpeer.c` contém a tabela de membros e o atendimento TCP antes separados em `membership.c` e `superpeer_app.c`. `client.c` foi removido: `bin/client` é apenas um alias de `bin/peer`. Upload/download são pedidos locais ao Peer ativo, não clientes TCP anônimos.
