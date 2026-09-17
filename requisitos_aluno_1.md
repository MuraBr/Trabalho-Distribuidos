# Requisitos do trabalho — Aluno 1

Este documento separa as responsabilidades do Aluno 1 a partir do enunciado do trabalho de Programação Distribuída.

## 1. Requisitos comuns da dupla

O projeto deve:

- ser implementado em linguagem C;
- funcionar em ambiente Linux;
- utilizar POSIX Sockets, TCP/IP e POSIX Threads;
- implementar comunicação cliente/servidor;
- utilizar serialização e framing de mensagens;
- utilizar CRC32 para validar mensagens;
- utilizar SHA-256 para identificadores e integridade de conteúdo;
- integrar-se com a implementação do Aluno 2;
- possuir testes, documentação e demonstração em cada checkpoint.

O sistema é um P2P híbrido para compartilhamento distribuído de documentos PDF, com peers e Super Peers.

### Header das mensagens

Todas as mensagens devem possuir os seguintes campos:

- versão do protocolo;
- tipo da mensagem;
- NodeID de origem;
- NodeID de destino;
- TransactionID;
- timestamp;
- tamanho do payload;
- checksum CRC32.

O receptor deve usar o tamanho do payload para determinar exatamente onde uma mensagem termina e a próxima começa.

Na integração atual, `JOIN` e `ACK` utilizam um payload de 64 bytes com IP
textual (46 bytes), porta em ordem de rede (2 bytes) e UUID (16 bytes).

### Integridade e identificadores

- CRC32: valida a mensagem completa, considerando header e payload;
- SHA-256: valida documentos e demais conteúdos definidos pelo protocolo;
- NodeID: possui 32 bytes e é calculado pelo Aluno 2 conforme `SHA256(IP || Porta || UUID)`;
- TransactionID: possui 16 bytes e deve ser tratado de forma consistente pelos dois alunos.

### Bibliotecas externas utilizadas

- O CRC32 é fornecido pela biblioteca `zlib` (`crc32()`); não há mais uma implementação manual do polinômio no projeto.
- O SHA-256 é fornecido pela `libcrypto` do OpenSSL (`SHA256()`), chamado diretamente em `node.c`, onde o `NodeID` é calculado.
- Neste ambiente, o link é feito explicitamente para `libcrypto.so.3`, pois a biblioteca de execução está instalada mesmo sem os headers de desenvolvimento do OpenSSL.

## 2. Responsabilidades do Aluno 1

### Arquivos principais

- `common.h`: constantes compartilhadas entre identidade e protocolo;
- `network.c`: operações de socket TCP;
- `protocol.c`: header, serialização, framing e CRC32;
- `peer.c`: processo principal do peer e fluxo cliente/servidor.

## 3. Checkpoint 1 — comunicação básica

O Aluno 1 deve implementar:

- criação de socket;
- `bind`;
- `listen`;
- `accept`;
- `connect`;
- envio com `send`;
- recebimento com `recv`;
- tratamento de envios e recebimentos parciais;
- serialização e desserialização do header;
- framing baseado no tamanho do payload;
- cálculo e validação do CRC32;
- concorrência básica, preferencialmente com POSIX Threads;
- comunicação entre pelo menos dois processos.

O `peer.c` deve conseguir iniciar como servidor e, quando necessário, conectar-se a outro peer como cliente.

O peer deve utilizar o `NodeID` produzido por `node.c`, copiando os 32 bytes
de `Node.id.bytes` para os campos do header.

### Mensagens mínimas

Para a primeira demonstração, devem funcionar pelo menos:

- `JOIN`;
- `ACK`;
- `ERROR`.

### Verificações do checkpoint 1

- conexão TCP funcionando;
- mensagem chegando corretamente;
- header interpretado corretamente;
- checksum validado;
- dois processos conversando;
- ausência de segmentation fault;
- sockets fechados em caminhos normais e de erro.

## 4. Checkpoint 2 — arquivos e transferência

Responsabilidades do Aluno 1:

- upload;
- download;
- armazenamento local;
- fragmentação de arquivos;
- SHA-256 do conteúdo;
- compressão e descompressão com LZ4;
- checksum dos blocos;
- transferência paralela.

O tamanho de chunk especificado no trabalho é 4 MB.

Pipeline esperado:

```text
arquivo → SHA-256 → fragmentação → LZ4 → checksum → transferência
```

## 5. Checkpoint 3 — membership e falhas

Responsabilidades do Aluno 1:

- manutenção de membership local;
- processamento de heartbeats;
- estado `SUSPECT`;
- estado `FAILED`;
- estado `REMOVED`.

Transição esperada:

```text
ALIVE → SUSPECT → FAILED → REMOVED
```

As mensagens de membership devem utilizar o protocolo definido em `protocol.c`.

## 6. Checkpoint 4 — eleição

Responsabilidades do Aluno 1:

- envio da mensagem `ELECTION`;
- recebimento da resposta `OK`;
- envio da mensagem `COORDINATOR`;
- fluxo de recuperação quando o coordenador falhar.

## 7. Checkpoint 5 — cache e transferência

Responsabilidades do Aluno 1:

- implementação do cache LFU;
- identificação de cache hit e cache miss;
- contagem de frequência de uso;
- eviction de entradas;
- transferência de dados usando a camada de rede.

## 8. Checkpoint 6 — integração final

O Aluno 1 deve demonstrar:

- upload;
- download;
- fragmentação em chunks;
- compressão;
- cache;
- réplica e comunicação com os componentes do Aluno 2.

## 9. Testes de integração com o Aluno 2

O peer deve:

1. enviar um `JOIN` contendo seu NodeID e sua configuração serializada;
2. permitir que o Super Peer reconstrua e valide o `Node`;
3. receber `ACK` quando o nó for registrado;
4. receber `ERROR` quando o NodeID, payload ou checksum forem inválidos;
5. manter o mesmo TransactionID na solicitação e na resposta.

Não se deve enviar diretamente uma `struct Node` pela rede. O payload deve possuir um formato de rede documentado, sem depender de padding ou representação interna das structs.

## 10. Requisitos não funcionais

O trabalho apresenta como metas:

- disponibilidade mínima de 99,5%;
- escalabilidade sem reconfiguração manual;
- consistência forte dos metadados;
- lookup médio em `O(log N)`;
- sincronização incremental em menos de 2 segundos;
- throughput de compressão LZ4 superior a 500 MB/s;
- eleição em menos de 5 segundos.

## 11. Comandos iniciais de teste

Compilação do Aluno 1:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -pthread \
    network.c protocol.c node.c superpeer.c peer.c \
    -Wl,-l:libcrypto.so.3 -lz -o peer
```

Execução de dois peers:

```bash
./peer 5000
./peer 5001 127.0.0.1 5000
```
