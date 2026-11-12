# Plano — Harness distribuído do Checkpoint 6 (ssh, TUI e clientes persistentes)

Fonte: [`docs/specification/trabalho_2026_SD.pdf`](../../specification/trabalho_2026_SD.pdf) (item 10, Checkpoint 6 — integração e teste final).

O CP6 pede que o Aluno 1 demonstre `upload`, `download`, `chunks`, `compression`,
`cache` e `replica`, e que o Aluno 2 demonstre `Chord`, `Gossip`, `Bully`, `SMR`,
`2PC` e `IST`. O professor vai fornecer um conjunto de testes para o sistema.

Até o CP5 todo teste rodou numa máquina só (`tests/flow/`, `tests/c1` … `tests/c5`),
e cada cliente fazia um comando e morria. Este plano descreve o harness novo, que:

1. sobe os nós em **máquinas diferentes, via ssh**, a partir de um arquivo de configuração;
2. tem uma **interface de terminal** no estilo de `tests/flow/watch.sh`;
3. mantém os **clientes abertos** até alguém pedir para fechar, recebendo comandos
   em modo **interativo** ou **batch**.

## Situação

O plano foi implementado na branch `feat/harness-distribuido`. O guia de uso está em
[`tests/cluster/README.md`](../../../tests/cluster/README.md). O que mudou em relação ao
texto original deste plano:

- **Escala por máquina.** Cada `host` do `cluster.conf` gera sozinho **3 Super Peers e 30
  clientes** (padrões `sp_per_host` e `clients_per_host`), com nomes globais `SP1`…`SPn`
  e `C1`…`Cm`. As portas vêm do próprio arquivo (`sp_port_base`, `sp_ports=` ou linhas
  `node` avulsas). O roster e o `.conf` de cada cliente são gerados a partir dele.
- **Número indeterminado de máquinas.** Basta acrescentar linhas `host`. O limite é o do
  parser de roster, 64 Super Peers (cerca de 21 máquinas com 3).
- **Senhas.** `cluster.sh copy-keys` copia a chave ssh com `ssh-copy-id` e `sshpass`,
  perguntando a senha (ou lendo `$SSHPASS`). A senha nunca é gravada. Depois disso todo o
  harness usa `BatchMode`.
- **Clientes.** `bin/client` ganhou `--interactive` e `--batch` (código em
  `src/peer/peer.c` e `src/peer/session.c`, com testes em `tests/peer/test_session.c`).
  O modo `--cmd` não mudou.
- **Espelho de log.** Um espelho por máquina, com lock, no lugar de um por processo, para
  caber 33 processos por máquina. As vidas de um processo são separadas por um marcador
  no próprio `proc.log`.
- **Saída do ssh.** O `rexec` descarta tudo o que a máquina remota imprime antes do
  comando (`.bashrc`, `motd`), porque isso corrompia caminhos e `sha256` capturados.
- **Cenários.** `smoke`, `final`, `carga` e `professor` (encaixe). O `carga` põe todos os
  clientes para trabalhar ao mesmo tempo.

Resultados dos ensaios, todos sem falha: `final` com 3 Super Peers numa máquina e com 6
em duas (via ssh), e `carga` com 60 clientes persistentes (30 por máquina) e 6 Super
Peers, em ondas de 10 e com todos de uma vez, terminando com o mesmo `LogIndex 60` nos 6
Super Peers. As máquinas desses ensaios eram duas "máquinas" simuladas (dois diretórios e
dois IPs de loopback acessados por ssh), não máquinas reais; o ensaio em máquinas reais
continua pendente. Uma métrica merece atenção: a eleição no boot levou de 5,0 a 5,5 s
nos ensaios, contra o alvo de 5 s do RNF-010 (medida do primeiro log do primeiro nó até a
vitória do coordenador, o que inclui a subida dos processos).

## O que existe hoje e o que isso impõe

Pontos do código atual que moldam o desenho (lidos em `src/peer/peer.c`,
`src/superpeer/superpeer.c`, `src/common/config.c` e `tests/flow/`):

| Fato hoje | Consequência para o harness |
| --- | --- |
| `bin/client` executa um `--cmd` e sai; o código de saída é o resultado. | Precisa de modo sessão. O one-shot continua existindo, porque `tests/flow/run.sh` e `tests/c*/run.sh` dependem dele. |
| O cliente gera um NodeID efêmero a cada execução. | Na sessão o NodeID nasce uma vez e vale para todos os comandos. |
| `node_roster_load` escolhe a linha do roster **só pela porta**. Duas linhas com a mesma porta falham. | As portas dos Super Peers têm de ser **únicas no cluster inteiro**, mesmo em máquinas diferentes. O `check` valida isso. Mudar o parser fica como opção (ver abaixo). |
| O NodeID do Super Peer é função de `ip` e `porta` da linha do roster; o bind é `INADDR_ANY`. | A coluna `ip` do roster tem de ser o IP real que as outras máquinas alcançam, nunca `127.0.0.1`. |
| Os Super Peers gravam chunks em `data/storage/<porta>` e o cliente em `storage/`, ambos **relativos ao diretório de trabalho**. | Cada processo remoto roda no próprio diretório (`run/<nome>/`). Dois clientes na mesma máquina não podem dividir `storage/`. |
| O harness atual confere os logs com `wait_log`, `assert_round`, `assert_state` etc. em `tests/flow/lib.sh`, lendo arquivos locais e carimbando o horário de cada linha. | Reaproveitar tudo isso. O cluster espelha os logs remotos para arquivos locais, com o mesmo formato. |
| O `bin/node` e o `bin/client` já saem do `make`; dependem de `libcrypto`, `zlib` e `liblz4`. | O `check` confere compilador e bibliotecas em cada máquina. |

## Visão geral da solução

```text
   máquina do operador (tests/cluster/)                    máquinas remotas
  ┌──────────────────────────────────────┐   ssh      ┌──────────────────────────┐
  │ cluster.conf ─► cluster.sh           │ ─────────► │ run/<nome>/  (cwd do nó) │
  │                  ├ sync / build      │            │   proc.log               │
  │                  ├ up / down / kill  │            │   pid                    │
  │                  ├ client send/wait  │            │   in.fifo  (só cliente)  │
  │ logs/<nome>.log ◄┤ espelho + carimbo │ ◄───────── │ bin/node | bin/client    │
  │ scenarios/*.sh ──┘ asserts do flow   │ tail -F    └──────────────────────────┘
  │ watch.sh (tmux)                      │
  └──────────────────────────────────────┘
```

Princípios:

- **Um único ponto de controle.** Toda ação passa por `cluster.sh`; os scripts de
  cenário e o `watch.sh` só chamam esse comando.
- **Estado remoto mínimo.** Cada máquina guarda só o código, o diretório `run/`
  e os processos. Tudo que o operador lê fica espelhado na máquina local.
- **Relógios independentes.** A spec assume relógios locais independentes. Por isso
  o carimbo de horário de cada linha de log é posto **na máquina do operador**, no
  momento em que a linha chega, e não na máquina remota. Medidas como "eleição em
  menos de 5 s" ou "IST em menos de 2 s" usam esse relógio único.
- **Compatível com o que existe.** Nada do harness atual é removido; o novo código
  vive em `tests/cluster/` e só reaproveita `tests/flow/lib.sh`.

## 1. Arquivo de configuração

`tests/cluster/cluster.conf` (ignorado pelo git, porque tem IPs e usuários da
equipe) a partir de `cluster.conf.example` (versionado). Formato por linha, com
`#` para comentário, fácil de ler em bash sem dependência externa:

```text
# ---- padrões ----
set sp_per_host      3
set clients_per_host 30
set sp_port_base     5100        # SP k (global, a partir de 1) usa a porta base+k
set client_port_base 9000        # só entra na identidade do cliente; ele não escuta
set remote_dir       ~/bittorrent-project
set ssh_opts         -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5

# ---- máquinas: host <apelido> <alvo-ssh|local> [ip=...] [dir=...] [sps=N] [clients=N] [sp_ports=...] ----
host m1  aluno@192.168.18.11
host m2  aluno@192.168.18.12
host m3  aluno@lab-pc3.local   ip=192.168.18.13
host m4  aluno@192.168.18.14   sps=2 clients=10 dir=/opt/bt

# ---- avulsos (opcional) ----
# node   <nome> superpeer <host> <porta> [prioridade]
# client <nome> <host> [super-peer-de-entrada] [port=P]
```

Regras:

- `host <apelido> local` roda sem ssh, na própria máquina. Serve para desenvolver o
  harness sem rede e como alvo do `check` nos testes iniciais. `localhost` via ssh
  também deve funcionar.
- `ip=` é o IP que vai para a coluna `ip` do roster. Sem ele, usa-se o trecho depois
  do `@` do alvo ssh, desde que seja um IPv4.
- A prioridade é a coluna `prioridade` do roster (maior vence o Bully); por padrão é a ordem global dos Super Peers.
- Senha não entra no arquivo. Autenticação por chave (`BatchMode=yes`). Se alguém
  precisar de senha, o `sshpass` já instalado cobre via variável de ambiente
  `SSHPASS`, documentado, nunca gravado.
- O `cluster.sh` **gera** o roster no formato que o código já lê
  (`nome,superpeer,ip,porta,id,prioridade`, `id` sequencial de 8 dígitos hex) em
  `tests/cluster/out/roster.conf` e o copia para `run/roster.conf` de todas as
  máquinas. Para cada cliente gera um `.conf` de `type=peer` com `bootstrap=` no
  Super Peer de entrada.

## 2. Estrutura de arquivos proposta

```text
tests/cluster/
├── README.md                 # guia de uso
├── cluster.conf.example      # exemplo versionado
├── cluster.sh                # CLI única (subcomandos abaixo)
├── watch.sh                  # TUI em tmux (análogo a tests/flow/watch.sh)
├── run.sh                    # executa um cenário (ex.: scenarios/final.sh)
├── lib/
│   ├── conf.sh               # lê cluster.conf, valida, gera roster e .conf de clientes
│   ├── transport.sh          # ssh/local: rexec, rcopy, ControlMaster
│   ├── remote.sh             # copiado para as máquinas: start/stop/kill/alive/send
│   ├── node.sh               # up/down/kill/start de Super Peer + espelho de log
│   ├── client.sh             # sessão persistente: start/send/wait/stop/batch
│   ├── steps.sh              # passos comuns dos cenários (boot, download conferido)
│   └── asserts.sh            # reutiliza tests/flow/lib.sh + sobrescritas
├── scenarios/
│   ├── smoke.sh              # sobe tudo, um upload/download, derruba
│   ├── final.sh              # roteiro do CP6 (seção 6)
│   └── professor.sh          # encaixe para os testes do professor
├── batch/
│   └── exemplo.cmds          # exemplo de arquivo de comandos de cliente
├── logs/                     # espelho local (gitignored)
└── out/                      # roster gerado, relatório, downloads (gitignored)
```

Entradas novas no `.gitignore`: `tests/cluster/cluster.conf`, `tests/cluster/logs/`,
`tests/cluster/out/`.

## 3. Camada de transporte e ciclo de vida remoto

### 3.1 `lib/transport.sh`

- `rexec <host> <comando...>`: roda no host (`ssh` ou, para `local`, direto).
  Usa `ssh -o ControlMaster=auto -o ControlPersist=60 -o ControlPath=...` para
  reaproveitar a conexão. Sem isso, cada `send` de cliente paga um handshake.
- `rcopy` / `rsync_to <host>`: sincroniza o projeto, excluindo `.git`, `.worktrees`,
  `bin/`, `obj/`, `data/`, `storage/` e `tests/cluster/{logs,out}`.
- Tempo limite em toda chamada (`timeout`), para um host caído não travar o harness.

### 3.2 `lib/remote.sh` (roda na máquina remota)

Um script pequeno, copiado junto com o código, para evitar problemas de aspas
em `ssh host "…"`. Subcomandos:

| Subcomando | O que faz |
| --- | --- |
| `start <nome> <cwd> <cmd...>` | `mkdir -p run/<nome>`, entra no diretório, lança com `setsid nohup stdbuf -oL -eL`, saída em `proc.log`, grava `pid`. Imprime o PID. |
| `start-client <nome> <cwd> <cmd...>` | Igual, mas cria `in.fifo` e abre com `exec 3<>in.fifo`, passando o fd 3 como stdin do cliente. |
| `alive <nome>` | Sai 0 se o PID do arquivo `pid` existe. |
| `stop <nome>` | `SIGTERM`, espera até 5 s, depois `SIGKILL`. |
| `kill9 <nome>` | `SIGKILL` imediato (injeção de falha, o `kill -9 SPx` da spec). |
| `send <nome> <linha>` | Escreve a linha em `in.fifo`. |
| `rotate <nome>` | Move `proc.log` para `proc.<n>.log` antes de uma nova vida do processo. |

`setsid` e arquivo de PID fazem o processo sobreviver à queda da sessão ssh e
permitem encerrar tudo depois, mesmo que o operador feche o terminal.

### 3.3 Espelho de log

Para cada nó e cada cliente, o harness mantém um processo local:

```text
ssh <host> 'tail -n +1 -F run/<nome>/proc.log' | stamp_stream >> logs/<nome>.<vida>.log
```

- `stamp_stream` é o de `tests/flow/lib.sh` (carimbo `HH:MM:SS.mmm`).
- `logs/<nome>.log` é um symlink para a vida atual. Quando o SP2 é derrubado e volta
  (IST), a nova vida vira `SP2.2.log` e o symlink passa para ela. É o mesmo papel
  de `sp5102-rejoin.log` no harness atual.
- `mirror_sync <nome>` compara `wc -l` remoto e local, com limite de 5 s, e só então
  devolve. As conferências chamam isso antes de ler o log, como o `run_client` do
  flow já faz com a FIFO para garantir que a última linha foi gravada.

## 4. Clientes persistentes

### 4.1 Lado do binário (`bin/client`) — **exige autorização de código**

Hoje o `main` de `src/peer/peer.c` faz `getopt_long`, despacha um `cmd_*` e sai.
Como cada `cmd_*` já devolve `int`, o modo sessão é um laço novo em cima deles:

- `--interactive` (`-I`): lê comandos da stdin, um por linha. Mostra prompt só se
  `isatty(0)`. Fecha em `quit`, em EOF ou em `SIGTERM`/`SIGINT`.
- `--batch ARQUIVO` (`-b`; `-` lê da stdin): mesma execução, sem prompt, termina no
  fim do arquivo. Sai com código diferente de zero se algum comando falhou.
  `--keep-going` continua depois de erro; o padrão é parar no primeiro.
- Gramática de linha, igual à CLI atual mas sem os `--`:
  `upload <arquivo>`, `download <nome> <saida>`, `lookup <nome|object-id>`,
  `put <nome> <valor>`, `state [host porta]`, `ping|join|leave [host porta]`,
  `sleep <segundos>`, `help`, `quit`. Linhas vazias e iniciadas por `#` são ignoradas.
  `--host` / `--port` da partida valem como Super Peer padrão da sessão.
- **NodeID de sessão:** gerado uma vez, impresso na partida (`NodeID: <hex>`) e usado
  em todos os comandos.
- **Marcadores para o harness** (stdout, sempre com flush; o cliente passa a usar
  `setvbuf` de linha, porque a saída vai para arquivo):

```text
NodeID: <hex>
[ready]
... saída normal do comando, as mesmas linhas de hoje ...
[done 3 rc=0]
[bye]
```

  `[done N rc=R]` substitui o código de saída do processo como sinal de "terminou" e
  de resultado. `N` é o número do comando na sessão.

- O modo one-shot (`--cmd`) continua igual, com o mesmo código de saída. Os testes
  existentes não mudam.
- Ficam fora do escopo desta mudança: novos tipos de mensagem, alterações no
  protocolo ou no Super Peer.

### 4.2 Lado do harness (`lib/client.sh`)

| Função | Comportamento |
| --- | --- |
| `client_start C1` | Gera o `.conf`, roda `remote.sh start-client` com `bin/client --interactive --config <conf>`, abre o espelho e espera `[ready]`. |
| `client_send C1 "<linha>"` | Incrementa o contador local de `C1` e escreve a linha no FIFO via `ssh`. Devolve o número `N`. |
| `client_wait C1 N [timeout]` | Espera `[done N rc=R]` no log espelhado. Devolve `R`. Esgotar o tempo é falha do cenário (`die`). |
| `client_do C1 "<linha>"` | `send` + `wait`; é o que os cenários usam. |
| `client_batch C1 arquivo` | Envia o arquivo linha a linha e espera o último `[done]`, ou executa `bin/client --batch` quando o cliente não está aberto. |
| `client_stop C1` | Envia `quit`, espera `[bye]`; se não vier em 5 s, `remote.sh stop`. |
| `client_attach C1` | Abre `ssh host 'cat > in.fifo'` para digitar direto no cliente (modo interativo humano). |

Um cliente fica aberto até `client_stop`, `down` ou o fim do cenário, que sempre
fecha o que abriu (`trap`).

### 4.3 Onde ficam os arquivos do cliente

Uploads leem de `run/files/` do host do cliente e downloads gravam em `run/<nome>/out/`.
`cluster.sh fixtures` copia `tests/data` (gerado por `tests/generate_fixtures.sh`)
para os hosts de cliente. A conferência byte a byte do download compara
`sha256sum` remoto contra o do arquivo de origem, sem trazer o arquivo de volta.

## 5. `cluster.sh` — interface de linha de comando

| Comando | Efeito |
| --- | --- |
| `check` | Valida o `cluster.conf` (portas únicas, nomes únicos, host de cada nó definido). Testa ssh, `gcc`, `make`, libs, relógio (mostra a diferença em segundos entre máquinas), portas livres e alcance TCP entre as máquinas dos nós. |
| `sync` | `rsync` do código para todos os hosts. |
| `build` | `make` em paralelo nos hosts (compila na máquina de destino, que a spec diz ser x86-64 heterogêneo). Falha clara por host. |
| `fixtures` | Envia os arquivos de teste aos hosts de cliente. |
| `up [nomes…]` | Gera e envia o roster e sobe os Super Peers. Sem argumento, todos, na ordem do arquivo. |
| `down` | Fecha clientes (`quit`), encerra Super Peers, fecha espelhos. Idempotente. |
| `kill <nome>` / `stop <nome>` / `start <nome>` | Injeção de falha (`SIGKILL`), saída ordenada e volta (nova vida, novo log). |
| `status` | Tabela: nome, host, `ip:porta`, vivo, papel (coordenador/follower, lido do log) e última linha. |
| `client <start\|send\|do\|wait\|batch\|attach\|stop> …` | Interface de `lib/client.sh`. |
| `exec <host> <cmd…>` | Atalho de depuração. |
| `logs <nome>` | `tail -F` do espelho local. |
| `clean [--data]` | Apaga logs locais e, com `--data`, `run/*/data` e `run/*/storage` nos hosts. |
| `run <cenário>` | Executa `scenarios/<cenário>.sh`. |
| `console` | Laço interativo do operador (seção 7). |

Todo subcomando devolve código de saída útil e imprime em pt-br com o mesmo prefixo
`HH:MM:SS.mmm [harness]` de `tests/flow/lib.sh`.

## 6. Cenários e cobertura do CP6

Cenários são scripts bash (como `tests/flow/run.sh`), que carregam `lib/asserts.sh`
e chamam `cluster.sh`. `lib/asserts.sh` faz `source tests/flow/lib.sh` e sobrescreve
só o que depende de local: `start_node`, `stop_pid`, `run_client` e `assert_state`.
`FLOW_LOG` aponta para `tests/cluster/logs`. As conferências de log (`assert_round`,
`assert_log_sequence`, `assert_lines_in_order`, `wait_log`) ficam como estão.

`scenarios/final.sh`, uma fase por item da lista do professor:

| Fase | Demonstra | Aluno | Como confere |
| --- | --- | --- | --- |
| 1 | Chord: `JOIN` de 5 Super Peers em máquinas diferentes e `lookup` com caminho e dono | 2 | `Successor ->` em todos; `lookup` pelo cliente mostra `Lookup path` e `Owner` |
| 2 | Bully: eleição no boot e depois de `kill -9` do coordenador | 2 | `highest NodeID wins`, `COORDINATOR <id>` em todos; tempo medido pelo carimbo local, alvo < 5 s |
| 3 | Gossip e detecção: `kill -9` de um SP | 2 | `SUSPECT` e depois `FAILED` no coordenador; tempos próximos de 5 s / 15 s |
| 4 | SMR e 2PC: uploads por clientes em máquinas diferentes | 2 | `assert_round` com os `YES` esperados; `assert_log_sequence` igual em todos; `state` com mesmo `LogIndex` |
| 5 | IST: SP volta depois de uploads feitos sem ele | 2 | `version 0 -> N`, `STATE_TRANSFER`, `LOG #` em ordem; alvo < 2 s |
| 6 | 2PC com falha em `PREPARE` | 2 | Usa o mesmo mecanismo de pausa do CP5; participantes não ficam inconsistentes |
| 7 | Upload, chunks e compressão | 1 | Saída do `upload` (`Chunks`, `Compression: LZ4`, `ObjectID`) e estados `CREATED` … `REPLICATED` |
| 8 | Download com `SHA-256 verified` | 1 | `Download completed`, `SHA-256 verified` e `sha256sum` igual ao original |
| 9 | Cache LFU | 1 | `cache miss`, `LFU frequency 1`, `cache hit`, `LFU frequency 2` só no SP consultado |
| 10 | Réplica: derruba o SP que guarda uma réplica e baixa de novo | 1 | Download continua correto, `Transfer … REPLICATED N` com N ≥ 2 |

No fim, `run.sh` imprime o resumo (`summary` do `tests/common.sh`) e grava
`out/relatorio.md`: tabela de fases com PASS/FAIL, tempos medidos, caminho dos logs.
Esse relatório alimenta o documento de entrega do CP6, que a spec exige.

`scenarios/professor.sh` fica como encaixe para o conjunto de testes do professor:
quando ele chegar, o cenário só precisa de `client_do` e `kill` para reproduzi-lo,
sem mexer no harness.

## 7. Interface de terminal

### 7.1 `watch.sh` (tmux)

Mesma ideia de `tests/flow/watch.sh`: uma sessão tmux (`bittorrent-cluster`),
com o roteiro no painel largo e os logs ao lado.

- Painel principal: o cenário (`CLUSTER_WATCH=1 bash tests/cluster/run.sh final`)
  ou, com `--console`, o `cluster.sh console`.
- Um painel por Super Peer: `tail -F logs/<nome>.log` do espelho local (sem
  conexão ssh extra por painel). O título mostra `NOME@host`.
- Um painel por cliente, mais um painel de status que roda `watch -n2 cluster.sh status`.
- Poucos nós cabem num layout `main-vertical`. Passando de 6 painéis, o script
  cria uma janela `sps` e outra `clientes`, em layout `tiled`.
- Os painéis são gerados a partir do `cluster.conf`, não de uma lista fixa como no
  `watch.sh` de hoje.
- `--attach` reabre a sessão existente; sem `tmux` ou dentro de outro tmux, mesma
  mensagem de erro do script atual.

### 7.2 `cluster.sh console`

Laço `read -e` com histórico. Linhas aceitas:

```text
C1> upload run/files/documento_a.pdf        # envia ao cliente C1 e espera [done]
C2> lookup documento_a.pdf
@SP2 kill                                   # kill -9
@SP2 start                                  # volta, nova vida
@SP3 stop
status
batch C1 tests/cluster/batch/exemplo.cmds
quit                                        # fecha tudo que o console abriu
```

`<Cliente>>` e `@<Nó>` são o único jeito de falar com os processos; assim o mesmo
vocabulário vale no console, nos cenários e na documentação.

## 8. Mudanças de código que precisam de autorização

A mudança 1 foi autorizada e feita (e a 2 junto com ela). A 3 não foi feita: as portas
únicas bastam.

| # | Mudança | Arquivos | Necessidade |
| --- | --- | --- | --- |
| 1 | Modo `--interactive` e `--batch`, NodeID de sessão, marcadores `[ready]`, `[done N rc=R]`, `[bye]`, `setvbuf` de linha | `src/peer/peer.c` (possivelmente um módulo novo `src/peer/session.c` + header em `include/peer/`, e `Makefile`) | **Obrigatória** para clientes persistentes |
| 2 | Testes do parser de linha da sessão | `tests/peer/` | Recomendada |
| 3 | `node_roster_load` casar por `ip` + `porta` (ou por `--name`) | `src/common/config.c`, `src/superpeer/superpeer.c`, testes | **Opcional.** Só se a equipe quiser a mesma porta em máquinas diferentes. Sem isso, portas únicas resolvem |

Se for feita a mudança 1, documentar com Doxygen em pt-br (skill `c-api-docs`).

## 9. Riscos e decisões

| Risco | Tratamento |
| --- | --- |
| Firewall ou rede do laboratório bloqueando as portas dos nós | `check` testa alcance TCP máquina a máquina antes de qualquer cenário |
| Relógios das máquinas diferentes | Carimbo local; `check` mostra a diferença e avisa acima de 2 s, porque o `Timestamp` do header da mensagem vem do relógio de cada nó |
| Processo remoto órfão se o ssh cair ou o operador sair | `setsid` + `pid` em arquivo; `down` e o `trap` do cenário encerram tudo; `status` mostra o que sobrou |
| Latência de ssh em cada comando de cliente | `ControlMaster` + `ControlPersist` |
| Cenário travado em host que não responde | `timeout` em todo `rexec` e em todo `wait`; `die` imprime nome do nó e padrão esperado |
| Dependências ausentes nas máquinas | `check` lista o que falta (compilador, `libcrypto`, `liblz4`, `zlib`) por host |
| Logs ficam grandes | `logs/` é gitignored; `clean` limpa |
| Estado de execução anterior contaminando o cenário | `up` do `final.sh` sempre começa com `clean --data` nos hosts |
| Particionamento de rede (premissa da spec) | Fora do primeiro corte. Se for preciso, gancho opcional em `remote.sh` com `iptables`, exigindo sudo; `kill -9` já cobre o roteiro da spec |

## 10. Ordem de trabalho

Cada etapa termina com algo executável. As três primeiras não dependem de mudança em `src/`.

1. **Base local.** `lib/conf.sh`, `lib/transport.sh`, `cluster.sh check`, com `host … local`.
2. **Nós remotos.** `remote.sh`, `sync`, `build`, `up`/`down`/`kill`/`start`/`status`, espelho de log com carimbo. Validar com `localhost` via ssh e depois com duas máquinas reais.
3. **Cenário sem cliente persistente.** `scenarios/smoke.sh`, usando o `bin/client` one-shot via `rexec`. Prova que roster, bootstrap e eleição funcionam entre máquinas.
4. **Cliente persistente** (depende da autorização do item 1 da seção 8). Modo `--interactive`/`--batch`, depois `lib/client.sh`.
5. **`final.sh`** com as dez fases, `relatorio.md` e medição de tempos.
6. **TUI.** `watch.sh` e `console`.
7. **Ensaios.** Rodar o `final.sh` em máquinas reais, 3 vezes seguidas, e corrigir o que for instabilidade do harness (não do sistema).
8. **Documentação.** Guia de uso em `docs/checkpoint-06-integration/deliverable/` e atualização do `README.md` (skill `readme-docs`).

Sugestão de divisão, coerente com 50 % / 50 % em integração e testes da spec:
Aluno 1 fica com o modo sessão do cliente (etapa 4) e as fases 7 a 10 do `final.sh`;
Aluno 2 fica com transporte, ciclo de vida dos nós e as fases 1 a 6. A TUI e o relatório
são de quem terminar primeiro.

## 11. Decisões em aberto

1. Ensaio em máquinas reais: o `cluster.conf` do laboratório ainda precisa ser preenchido,
   e o `check` (relógio, firewall, bibliotecas) rodado nelas.
2. A eleição no boot passou do alvo de 5 s nos ensaios locais (5,0 a 5,5 s). Vale medir de
   novo em máquinas reais antes de concluir se é problema do sistema ou da medição.
3. Em que momento isto vira PR para `dev`: a branch é `feat/harness-distribuido`.
