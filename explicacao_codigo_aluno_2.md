# Guia de apresentação — código do aluno 2

Revisão: 17/09/2026. Este guia descreve o código existente, principalmente o checkpoint 1. As pendências estão em [requisitos_aluno_2.md](requisitos_aluno_2.md).

## 1. O que minha parte faz

“Minha parte cria a identidade dos nós e mantém o cadastro de membros do Super Peer. Cada nó possui configuração, um identificador SHA-256 e informações locais do processo. O Super Peer usa esse identificador para adicionar, consultar, atualizar e remover membros, protegendo a tabela contra acessos simultâneos.”

Os arquivos centrais são `node.c` e `superpeer.c`. `node.h` e `superpeer.h` apresentam as estruturas e funções públicas; `common.h` compartilha os tamanhos com o protocolo. `test_node_superpeer.c` verifica os módulos. `peer.c`, da integração com o aluno 1, conecta essas funções às mensagens TCP; ele é explicado aqui apenas para mostrar como as partes se encontram.

## 2. Estruturas e conceitos

| Estrutura/campo | Significado |
| --- | --- |
| `NodeID` | 32 bytes, resultado do SHA-256; identidade usada no cadastro |
| `NodeConfig` | IP textual, porta `uint16_t` e UUID de 16 bytes |
| `Node` | Reúne ID, configuração, PID e papel |
| `NodeRole` | Distingue peer de Super Peer |
| `SuperPeerConfig` | Configuração do nó e capacidade inicial da tabela |
| `SuperPeerMember` | Cópia do nó, estado ALIVE e instante `last_seen` |
| `SuperPeer` | Estrutura privada com nó local, vetor de membros, contagem, capacidade e mutex |

O IP tem espaço para 46 bytes, suficiente para texto IPv6 e terminador. O NodeID ocupa 32 bytes binários, mas sua exibição exige 64 caracteres hexadecimais e um terminador: buffer de 65 bytes.

O UUID distingue instâncias com o mesmo endereço e porta. O PID identifica o processo no sistema operacional local; ele não é a identidade distribuída e não entra no hash. Quando `node_init` reconstrói um nó recebido, registra o PID do processo receptor: o protocolo não transporta o PID remoto.

## 3. `node.c`: função por função

### `normalize_ip`

É uma função interna (`static`). Usa `inet_pton` para validar e converter texto em endereço binário, primeiro IPv4 e depois IPv6. Usa `inet_ntop` para produzir uma representação textual normalizada. Rejeita ponteiros nulos, texto vazio e endereços inválidos. Isso evita depender da grafia textual do IP.

### `node_generate_uuid`

Abre `/dev/urandom` e lê até completar 16 bytes. Uma leitura pode devolver menos bytes que o solicitado; por isso existe um laço com `total_read`. Se um sinal interromper a leitura (`EINTR`), tenta novamente. Em erro, fecha o descritor e preserva a causa em `errno`.

Após a leitura, ajusta bits nos bytes 6 e 8 para identificar UUID versão 4 e sua variante. A aleatoriedade vem do sistema operacional. Não há arquivo de persistência: gerar outro UUID em uma nova execução também muda o NodeID.

### Configuração: `node_config_validate`, `node_config_init_with_uuid` e `node_config_init`

`node_config_validate` exige porta diferente de zero e procura o terminador `\0` dentro do vetor de IP antes de interpretar a string. Isso evita ler além do campo ao receber uma estrutura malformada. Depois valida o IP.

`node_config_init_with_uuid` normaliza o IP, zera a estrutura e copia endereço, porta e UUID informado. É usada em testes determinísticos e para reconstruir descritores recebidos pela rede. Ela aceita os 16 bytes fornecidos; não exige que um UUID externo tenha os bits de versão 4.

`node_config_init` gera um UUID e delega à versão anterior. Como a porta já chega como `uint16_t`, a validação de um texto numérico fora de 1–65535 pertence à camada que lê os argumentos, antes da conversão.

### `node_compute_id`: formação da identidade

```text
IP binário (4 ou 16 bytes) + porta (2 bytes, ordem de rede) + UUID (16 bytes)
                                 ↓ SHA-256
                           NodeID (32 bytes)
```

O código valida a configuração, converte o IP para bytes e usa `htons` na porta. A ordem de rede coloca o byte mais significativo primeiro e torna a entrada independente da ordem de bytes da máquina. `memcpy` concatena os campos em um buffer; o hash considera apenas os bytes preenchidos: 22 para IPv4 ou 34 para IPv6.

A função `SHA256` pertence à biblioteca OpenSSL/libcrypto. O código declara sua assinatura diretamente porque o ambiente original tinha `libcrypto.so.3` sem os headers de desenvolvimento. Não há algoritmo SHA-256 implementado manualmente.

Mesmos IP binário, porta e UUID produzem o mesmo ID. Alterar essas entradas deve alterar o hash, embora hashes não ofereçam garantia matemática de inexistência de colisões. PID e papel ficam fora da entrada. Validar esse hash confirma coerência entre dados e ID, mas não autentica a pessoa ou a máquina que enviou os dados.

### `node_init` e `node_validate`

`node_init` calcula o ID, copia a configuração, armazena `getpid()` e inicia o papel como `NODE_ROLE_PEER`. `superpeer_create` muda o papel de sua cópia local para Super Peer depois.

`node_validate` recalcula o ID esperado e compara com o armazenado. Também exige PID positivo e um dos dois papéis permitidos. Um nó com ID adulterado é rejeitado antes de entrar na tabela.

### Conversão e comparação de IDs

- `node_id_to_hex`: transforma cada byte em dois dígitos usando os grupos de quatro bits; verifica o espaço do buffer.
- `hexadecimal_value`: converte um caractere de `0–9`, `a–f` ou `A–F` em valor de 0 a 15; outros retornam -1.
- `node_id_from_hex`: exige 64 caracteres e combina cada par em um byte. Em falha, não se deve usar a saída como ID válido, pois ela pode ter sido parcialmente escrita.
- `node_id_compare`: usa comparação lexicográfica dos bytes e retorna -1, 0 ou 1. Trata ponteiro nulo como menor que não nulo. É uma base possível para ordenar prioridades, mas não é uma implementação de Bully.
- `node_id_equal`: exige dois ponteiros válidos e compara todos os bytes.
- `node_get_process_id`: devolve o PID armazenado ou -1 para ponteiro nulo.

## 4. `superpeer.c`: função por função

### Encapsulamento e concorrência

A definição completa de `SuperPeer` fica no `.c`; o `.h` expõe um tipo incompleto. Quem usa o módulo chama sua API sem manipular diretamente os campos internos.

`lock_members` e `unlock_members` encapsulam o mutex. Funções pthread devolvem o número do erro diretamente; esses auxiliares o colocam em `errno` e retornam -1, acompanhando a convenção do módulo.

O mutex protege busca, expansão e alteração da tabela, inclusive contagem. Assim, uma thread não deve consultar o vetor enquanto outra o move com `realloc`. A segurança depende de respeitar o ciclo de vida: destruir o objeto enquanto outra thread o usa continua sendo incorreto.

### `find_member_index_locked` e `grow_members_locked`

A busca percorre os membros e compara NodeIDs; é O(N). O sufixo `locked` indica que o chamador já deve estar com o mutex adquirido.

O crescimento só acontece quando não há espaço. A capacidade padrão é 16 e depois dobra. Antes da alocação, há verificações de overflow tanto na duplicação quanto na multiplicação pelo tamanho de cada membro. `realloc` usa um ponteiro temporário: se falhar, o vetor antigo continua disponível.

Essa tabela é um vetor dinâmico, não uma hash table nem uma DHT. A meta futura de lookup O(log N) não é satisfeita pela busca atual.

### `set_member`

Copia o `Node`, define `ALIVE` e grava `time(NULL)` em `last_seen`. É chamada tanto na inclusão quanto na atualização. Esse horário representa registro/renovação local; não existe rotina que envie heartbeats, expire membros ou faça transição para SUSPECT/FAILED.

### Configuração e criação

`superpeer_config_init_with_uuid` reaproveita a configuração com UUID conhecido; `superpeer_config_init` gera um UUID. Ambas definem capacidade inicial 16.

`superpeer_create` valida entradas, deixa `*output` inicialmente nulo, cria o nó local, aloca objeto e vetor, inicia o mutex e muda o papel local para `NODE_ROLE_SUPERPEER`. Registra esse nó no índice zero como ALIVE: a contagem começa em **1**, e não em zero. Capacidade configurada como zero usa o padrão. Cada falha de criação libera os recursos já adquiridos.

### `superpeer_register_node`

Primeiro chama `node_validate`. Depois adquire o mutex e busca o NodeID. Se já existir, substitui os dados e renova o horário, retornando `SUPERPEER_MEMBER_UPDATED` (0). Se não existir, aumenta o vetor quando necessário, grava o membro, incrementa a contagem e retorna `SUPERPEER_MEMBER_ADDED` (1). Em erro retorna `SUPERPEER_REGISTER_ERROR` (-1).

Exemplo: o Super Peer sozinho tem contagem 1. Cadastrar A leva a 2. Cadastrar A novamente mantém 2 e renova seu registro. Como a chave é o NodeID, uma configuração que resulte em outro ID representa outro membro.

### `superpeer_unregister_node`

Rejeita a remoção do próprio nó com `EPERM`. Para outro ID, busca sob mutex, retorna `ENOENT` se não encontrar e usa `memmove` para deslocar os elementos posteriores. `memmove` permite sobreposição entre origem e destino. Reduz a contagem, mas não reduz a capacidade alocada.

Essa remoção funciona na API local e está testada. O ramo LEAVE de `peer.c` ainda apenas envia ACK, sem chamar a função; não se deve apresentar LEAVE/ACK como prova de remoção.

### Consultas e destruição

- `superpeer_get_node`: copia o nó local, que não muda após a criação.
- `superpeer_find_member`: busca sob mutex e devolve uma cópia do membro. Isso evita devolver um endereço interno invalidado por expansão ou remoção.
- `superpeer_member_count`: lê a contagem protegida, incluindo o próprio nó. Retorno zero também pode significar erro.
- `superpeer_is_registered`: informa se encontrou o ID. Zero pode ser ausência ou erro; a API não distingue esses casos apenas pelo retorno.
- `superpeer_destroy`: destrói mutex e libera vetor e objeto; aceita nulo. As threads que usam o objeto precisam ter terminado antes.

Nas consultas com argumento `const`, o código faz um cast para adquirir o mutex: a leitura não modifica logicamente a tabela, mas sincronizar envolve alterar o estado interno do mutex.

## 5. Como o aluno 2 se integra ao JOIN

O descritor transmitido possui 64 bytes: IP textual e preenchimento zero (46), porta em ordem de rede (2) e UUID (16). Nunca se envia a memória bruta de `Node`: ela contém informações locais e pode ter padding dependente do compilador.

1. `peer.c` recebe a mensagem pela camada de protocolo do aluno 1, que cuida de framing e CRC32.
2. `register_join` verifica o destino: aceita o ID local ou destino zerado.
3. `decode_join_payload` valida tamanho, terminador e preenchimento; reconstrói `NodeConfig` com o UUID recebido.
4. `node_init` calcula o NodeID desse descritor.
5. O código compara o ID calculado com `source_node` e rejeita a identidade do próprio receptor.
6. `superpeer_register_node` inclui ou atualiza a tabela.
7. Só depois do registro a resposta é ACK, com o descritor local; falhas nesse processamento geram ERROR.

O ACK preserva o TransactionID do pedido. No fluxo de conexão inicial entre dois processos `bin/node`, quem recebe o ACK também valida a identidade e registra o outro nó. A inicialização reutiliza o mesmo UUID para o `Node` local e para o objeto `SuperPeer`, preservando o mesmo ID. Cada processo atualmente cria sua própria tabela de Super Peer; não há seleção distribuída de Super Peers implementada.

Se o CRC falhar durante a recepção, a implementação atual encerra o tratamento da conexão, sem enviar ERROR. Isso ainda difere do requisito escrito. Também há suporte a IPv6 na identidade, mas isso por si só não comprova suporte IPv6 em toda a camada TCP.

## 6. Testes e demonstração

`test_node_superpeer.c` contém seis cenários. `main` executa todos com `assert` e imprime `node/superpeer tests: ok` ao final. O arquivo desativa `NDEBUG` para manter as asserções ativas.

| Teste | O que demonstra |
| --- | --- |
| `test_node_id_is_deterministic` | Hash conhecido para configuração fixa |
| `test_node_records_process_and_round_trips_id` | PID, papel, identidade IPv6, ida e volta em hexadecimal, comparação de IDs e buffer insuficiente |
| `test_node_rejects_invalid_configuration` | IP inválido, porta zero, IP sem terminador e configuração com UUID gerado |
| `test_superpeer_registers_and_finds_members` | Configuração com UUID aleatório, autorregistro, expansão, inclusão, consulta e duplicata |
| `test_superpeer_rejects_inconsistent_nodes_and_unregisters` | ID adulterado, proteção do nó local, remoção e ausência |
| `test_superpeer_members_are_thread_safe` | Quatro threads registram 25 membros cada; total esperado 101 |

O auxiliar `register_members` gera identidades distintas por thread e registra os 25 nós. A capacidade inicial 1 força expansões durante o teste concorrente. `pthread_join` espera as threads antes de verificar resultados e destruir o objeto. Esse teste exercita o mutex, mas não prova ausência de todas as condições de corrida nem testa remoções concorrentes.

Na raiz do projeto, compile e execute a suíte do aluno 2 sem sobrescrever binários versionados:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -pthread \
    node.c superpeer.c test_node_superpeer.c \
    -Wl,-l:libcrypto.so.3 -o /tmp/aluno2-test
/tmp/aluno2-test
```

Essa suíte passou nesta revisão. Para a demonstração TCP com GCC 13:

```bash
make -B CFLAGS='-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion'
bash script_testes.sh
```

O script inicia e encerra seu próprio servidor. Ele testa o protocolo C1, incluindo JOIN/ACK, mas não executa a suíte do aluno 2. Nesta revisão, o script oficial terminou com 10 testes aprovados e o script peer-to-peer terminou com 14 testes aprovados. O arquivo de log padrão é `tests/c1/logs/node_55101.log`.

Para mostrar dois processos registrando um ao outro, use dois terminais, sem o script rodando nas mesmas portas:

```bash
# Terminal 1
./bin/node 5000
```

```bash
# Terminal 2
./bin/node 5001 127.0.0.1 5000
```

Procure `JOIN validado` no receptor e `JOIN aceito pelo peer remoto` no iniciador, acompanhados pela contagem de membros. Encerre com Ctrl+C. Esses são comandos para demonstração; não foram executados nesta revisão documental.

## 7. Roteiro curto para falar na apresentação

“Começo pela configuração: valido IP e porta e obtenho um UUID. Depois calculo SHA-256 do endereço binário, da porta em ordem de rede e do UUID. Isso produz os 32 bytes do NodeID, sem depender do PID local.”

“O Super Peer nasce com seu próprio nó cadastrado. A tabela é um vetor que cresce conforme necessário. O cadastro usa NodeID como chave: se o membro já existe, atualizo o registro; se não existe, adiciono. Um mutex protege a tabela quando várias threads chegam juntas.”

“Na integração, o aluno 1 recebe e desserializa a mensagem. A configuração recebida permite recalcular o NodeID e compará-lo com o header. Só após validar e registrar o membro a aplicação responde ACK.”

“Os testes verificam identidade, validação, duplicatas, remoção e registros concorrentes. O que está pronto é a base de identidade e cadastro do checkpoint 1. A remoção via rede ainda precisa ser ligada à API. Metadados, Chord, Gossip, detecção de falhas, eleição, SMR, 2PC e IST continuam pendentes.”
