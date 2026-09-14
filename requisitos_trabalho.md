# Trabalho de Programação Distribuída

## Implementação de um BitTorrent distribuído

Este arquivo resume os requisitos, as normas de desenvolvimento e a divisão de responsabilidades do trabalho descrito no enunciado.

> **Fonte:** `trabalho_2026_SD.pdf`  
> **Disciplina:** Programação Distribuída  
> **Curso:** Ciência da Computação  
> **Instituição:** Universidade Estadual de Mato Grosso do Sul (UEMS)  
> **Data final:** 06/11/2026

## 1. Normas gerais

- O trabalho deve ser realizado em dupla.
- É permitido utilizar ferramentas de inteligência artificial.
- A duração prevista do trabalho é de oito semanas.
- O trabalho é dividido em seis checkpoints.
- Cada checkpoint deve estar funcional antes da apresentação do checkpoint seguinte.
- Se um checkpoint não estiver funcional, a dupla deverá apresentar novamente o checkpoint anterior.
- Ao final de cada checkpoint, a dupla deve entregar um documento explicando o desenvolvimento realizado.
- No dia de cada checkpoint, o aluno responsável pela parte correspondente deve fazer a apresentação.
- A integração, os testes e a documentação são responsabilidades compartilhadas igualmente.

### Datas dos checkpoints

| Checkpoint | Data |
|---|---|
| 1 | 18/09/2026 |
| 2 | 30/09/2026 |
| 3 | 09/10/2026 |
| 4 | 21/10/2026 |
| 5 | 30/10/2026 |
| 6 | 06/11/2026 |

## 2. Objetivo do sistema

O projeto consiste em um sistema P2P híbrido para compartilhamento distribuído de documentos PDF.

O sistema combina:

- uma rede estruturada baseada em DHT-Chord;
- uma camada hierárquica de Super Peers;
- Gossip Protocol para descoberta, disseminação e detecção de falhas;
- SHA-256 para identificação e integridade de documentos;
- CRC32 para integridade das mensagens;
- compressão LZ4;
- cache LFU;
- eleição de coordenador pelo algoritmo Bully;
- Two-Phase Commit (2PC);
- State Machine Replication (SMR);
- Incremental State Transfer (IST).

## 3. Restrições técnicas obrigatórias

O sistema deverá:

- ser implementado em linguagem C;
- operar em ambiente Linux;
- utilizar POSIX Sockets;
- utilizar POSIX Threads;
- utilizar TCP/IP;
- utilizar SHA-256;
- utilizar LZ4;
- utilizar Hash Tables;
- implementar ou integrar DHT-Chord;
- implementar Gossip Protocol;
- implementar o algoritmo Bully;
- implementar 2PC;
- implementar State Machine Replication;
- implementar Incremental State Transfer;
- suportar arquiteturas x86-64;
- manter independência de banco de dados relacional.

## 4. Arquitetura do sistema

O sistema é organizado nas seguintes camadas:

```text
Application Layer
        ↓
Service Layer
        ↓
Metadata Layer
        ↓
Replication Layer
        ↓
Consensus Layer
        ↓
Compression Layer
        ↓
Network Layer
        ↓
TCP/IP
```

Os principais subsistemas são:

1. Cliente P2P;
2. Super Peer;
3. Overlay DHT-Chord;
4. Gossip Manager;
5. Metadata Manager;
6. Consensus Manager;
7. Replication Manager;
8. Cache Manager;
9. Compression Manager.

### Atores

#### Usuário

- compartilha documentos;
- pesquisa documentos;
- realiza downloads;
- remove documentos.

#### Peer

- armazena documentos;
- responde a pesquisas;
- envia e recebe blocos;
- atualiza metadados.

#### Super Peer

- mantém índices locais;
- gerencia cache;
- executa Gossip;
- participa da DHT;
- executa replicação e consenso;
- participa das eleições.

#### Coordenador

- coordena transações 2PC;
- supervisiona a replicação;
- registra Super Peers;
- detecta inconsistências;
- inicia recuperação.

## 5. Modelo de comunicação

Todas as mensagens devem utilizar uma estrutura padronizada composta por header e payload.

### Campos do header

- versão do protocolo;
- tipo da mensagem;
- nó de origem;
- nó de destino;
- Transaction ID;
- timestamp;
- tamanho do payload;
- checksum CRC32.

O framing deve permitir que o receptor saiba exatamente onde uma mensagem termina e a próxima começa. O tamanho do payload deve ser informado no header.

### Tipos de mensagem previstos

| Mensagem | Finalidade |
|---|---|
| `JOIN` | Entrada de nó |
| `LEAVE` | Saída de nó |
| `LOOKUP` | Pesquisa |
| `STORE` | Registro |
| `DOWNLOAD_REQ` | Solicitação de download |
| `DOWNLOAD_REP` | Resposta de download |
| `PREPARE` | Fase Prepare do 2PC |
| `COMMIT` | Confirmação do 2PC |
| `ABORT` | Cancelamento do 2PC |
| `HEARTBEAT` | Monitoramento |
| `GOSSIP` | Disseminação de estado |
| `ELECTION` | Início de eleição Bully |
| `OK` | Resposta de eleição |
| `COORDINATOR` | Divulgação do novo coordenador |
| `SNAPSHOT` | Replicação de estado |
| `STATE_TRANSFER` | Sincronização incremental |
| `ACK` | Confirmação |
| `ERROR` | Erro |

### Integridade

- CRC32: integridade da mensagem inteira;
- SHA-256: integridade do conteúdo ou documento.

### Heartbeats

- intervalo sugerido: 5 segundos;
- timeout: 15 segundos.

## 6. Identificadores

### NodeID

Cada Super Peer recebe um NodeID calculado conceitualmente como:

```text
NodeID = SHA256(IP || Porta || UUID)
```

O NodeID pertence ao espaço de identificadores de 256 bits do Chord.

### ObjectID

Cada documento recebe um identificador calculado como:

```text
ObjectID = SHA256(Arquivo)
```

### TransactionID

O enunciado define TransactionID de 128 bits e menciona timestamp, NodeID e sequence number em sua composição. A representação exata deve ser combinada pela dupla antes da integração, pois o NodeID possui 256 bits.

## 7. Responsabilidades por aluno

| Área | Aluno 1 | Aluno 2 |
|---|---|---|
| Protocolo de mensagens | Responsável | Apoio |
| TCP/Sockets | Responsável | Apoio |
| Cliente P2P | Responsável | — |
| Upload/download | Responsável | Apoio |
| Fragmentação | Responsável | — |
| SHA-256 | Responsável | — |
| LZ4 | Responsável | — |
| Armazenamento de arquivos | Responsável | — |
| LFU Cache | Responsável | — |
| Metadata | Apoio | Responsável |
| Hash table | Apoio | Responsável |
| Super Peer | Apoio | Responsável |
| Chord | — | Responsável |
| Finger Table | — | Responsável |
| Gossip | — | Responsável |
| Heartbeat | — | Responsável |
| Detecção de falhas | — | Responsável |
| Bully | — | Responsável |
| SMR | — | Responsável |
| 2PC | — | Responsável |
| IST | — | Responsável |
| Integração | 50% | 50% |
| Testes | 50% | 50% |
| Documentação | 50% | 50% |

## 8. Checkpoint 1 — comunicação básica

### Requisitos gerais

Devem estar implementados:

- processo distribuído;
- socket TCP;
- funcionamento cliente/servidor;
- serialização;
- framing de mensagens;
- concorrência básica;
- identificação de nós.

### Aluno 1

O aluno 1 implementa:

- `network.c`;
- `protocol.c`;
- `peer.c`.

Esses módulos devem realizar:

- criação do socket;
- `bind`;
- `listen`;
- `accept`;
- `connect`;
- `send`;
- `recv`;
- framing;
- header;
- checksum CRC32.

### Aluno 2

O aluno 2 implementa:

- `node.c`;
- `superpeer.c`.

Esses módulos devem realizar:

- geração ou gerenciamento do NodeID;
- configuração;
- identificação do processo;
- registro do nó;
- tabela básica de membros;
- criação do Super Peer.

### Verificações do checkpoint 1

- conexão TCP funcionando;
- mensagem chegando corretamente;
- header interpretado corretamente;
- checksum validado;
- dois processos conversando;
- ausência de segmentation fault.

## 9. Checkpoint 2 — arquivos e transferência

### Aluno 1

- upload;
- download;
- armazenamento;
- fragmentação;
- SHA-256;
- LZ4;
- transferência paralela.

Pipeline esperado:

```text
arquivo
  ↓
SHA-256
  ↓
fragmentação
  ↓
LZ4
  ↓
checksum
  ↓
transferência
```

O tamanho de chunk especificado é 4 MB.

### Aluno 2

- metadata;
- hash table;
- ObjectID;
- registro dos chunks.

## 10. Checkpoint 3 — membership, Gossip e Chord

### Aluno 1

- membership;
- heartbeat;
- estado `SUSPECT`;
- estado `FAILED`;
- estado `REMOVED`.

Estados previstos:

```text
ALIVE → SUSPECT → FAILED → REMOVED
```

### Aluno 2

- successor;
- predecessor;
- finger table;
- lookup;
- join;
- stabilize;
- notify;
- fix_fingers.

## 11. Checkpoint 4 — eleição e replicação

### Aluno 1

- mensagens `ELECTION`;
- resposta `OK`;
- mensagem `COORDINATOR`;
- fluxo de recuperação do coordenador.

### Aluno 2

- operation log;
- log index;
- transaction ID;
- version;
- checksum do log.

O maior NodeID deve possuir a maior prioridade na eleição Bully.

## 12. Checkpoint 5 — cache, transferência e consistência

### Aluno 1

- LFU Cache;
- cache hit;
- cache miss;
- contagem de frequência;
- eviction;
- transferência.

### Aluno 2

- 2PC;
- mensagens `PREPARE`, `COMMIT` e `ABORT`;
- Incremental State Transfer;
- recuperação de versões ausentes.

## 13. Checkpoint 6 — integração final

### Demonstração do aluno 1

- upload;
- download;
- chunks;
- compressão;
- cache;
- réplica.

### Demonstração do aluno 2

- Chord;
- Gossip;
- Bully;
- SMR;
- 2PC;
- IST.

### Demonstração conjunta

- integração;
- debugging;
- documentação;
- testes.

## 14. Requisitos não funcionais

O enunciado apresenta as seguintes metas:

- disponibilidade mínima de 99,5%;
- escalabilidade sem reconfiguração manual;
- consistência forte dos metadados;
- validação de integridade com SHA-256;
- lookup médio em `O(log N)`;
- sincronização incremental em menos de 2 segundos;
- throughput de compressão LZ4 superior a 500 MB/s;
- eleição em menos de 5 segundos.

## 15. Checklist geral de apresentação

- [ ] O projeto compila em Linux.
- [ ] Os processos conseguem iniciar e encerrar corretamente.
- [ ] Os sockets são fechados em caminhos de erro.
- [ ] Mensagens parciais são tratadas corretamente.
- [ ] O header é serializado e interpretado de forma determinística.
- [ ] O CRC32 é calculado e validado pelos dois lados.
- [ ] O NodeID é tratado de forma consistente entre os módulos.
- [ ] Há testes para desconexão, payload inválido e checksum incorreto.
- [ ] Não ocorre segmentation fault nos cenários demonstrados.
- [ ] A documentação do checkpoint está atualizada.

