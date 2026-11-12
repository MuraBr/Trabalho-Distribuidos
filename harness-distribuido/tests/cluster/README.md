# Harness distribuído (CP6)

Sobe Super Peers e clientes em várias máquinas por ssh, a partir de um arquivo
de configuração, e roda os cenários de demonstração do Checkpoint 6. O plano e
as decisões estão em
[`docs/checkpoint-06-integration/plans/harness-distribuido.md`](../../docs/checkpoint-06-integration/plans/harness-distribuido.md).

Os clientes **não morrem depois de um comando**: cada um é um
`bin/client --interactive` que fica aberto até receber `quit`, e aceita comandos
em modo interativo ou em lote.

## Requisitos

- Na máquina do operador: `bash` 5, `ssh`, `tar`, `tmux` (só para a TUI) e, para copiar
  as chaves, `ssh-copy-id` e `sshpass`.
- Em cada máquina: `gcc`, `make`, `libssl`, `liblz4`, `zlib`, `stdbuf`, `setsid` (o
  `check` confere). Linux x86-64, como pede a spec.
- As máquinas se enxergam na mesma rede, e as portas dos Super Peers estão liberadas.

## Primeiros passos

```bash
cp tests/cluster/cluster.conf.example tests/cluster/cluster.conf   # ajuste hosts e usuários
tests/cluster/cluster.sh list          # confere a topologia gerada
tests/cluster/cluster.sh copy-keys     # copia a chave ssh (pede a senha, não grava)
tests/cluster/cluster.sh check         # ssh, compilador, bibliotecas, relógio, portas
tests/cluster/cluster.sh deploy        # envia o código, compila e envia os arquivos de teste
tests/cluster/run.sh smoke             # confere rede, ssh e roster
tests/cluster/watch.sh final           # cenário final com painéis em tmux
```

`copy-keys` gera `~/.ssh/id_ed25519` se não houver chave, pergunta a senha uma vez
(ou usa `$SSHPASS`) e roda `ssh-copy-id` nas máquinas em que a chave ainda não
funciona. Use `--per-host` se cada máquina tem uma senha. A senha nunca vai para
arquivo.

## `cluster.conf`

Cada `host` gera sozinho os Super Peers e os clientes dele. Os padrões são
**3 Super Peers e 30 clientes por máquina**:

```text
set sp_per_host      3
set clients_per_host 30
set sp_port_base     5100        # SP k (global) usa a porta base+k
set client_port_base 9000
host m1  aluno@192.168.18.11
host m2  aluno@192.168.18.12
host m3  aluno@lab-pc3.local  ip=192.168.18.13  sps=2  clients=10
host m4  aluno@192.168.18.14  sp_ports=5201,5202,5203
```

Os nomes são globais (`SP1`…`SPn`, `C1`…`Cm`). As portas vêm do arquivo: pela base,
por `sp_ports=` ou por linhas `node` avulsas. O roster dos Super Peers e o `.conf` de
cada cliente são **gerados** em `tests/cluster/out/stage/<host>/` e enviados às
máquinas; ninguém edita esses arquivos. Detalhes de cada diretiva estão em
`cluster.conf.example`.

Limites que o `check` e a leitura do arquivo impõem:

- As portas dos Super Peers são únicas no cluster inteiro, mesmo em máquinas
  diferentes: o roster identifica o nó só pela porta (`node_roster_load`).
- No máximo 64 Super Peers (o parser de roster), ou seja, cerca de 21 máquinas com 3.
- A coluna `ip` do roster é o IP que as outras máquinas usam. Defina `ip=` quando o nome
  do alvo ssh não resolve para ele.

## Comandos

| Comando | Efeito |
| --- | --- |
| `list`, `check`, `net` | Topologia, diagnóstico das máquinas e alcance TCP até cada Super Peer |
| `sync`, `build`, `fixtures`, `deploy` | Código, compilação e arquivos de teste nas máquinas |
| `up [--clients] [--keep]` | Zera o estado, sobe os Super Peers (e os clientes) |
| `down` | Fecha clientes e Super Peers, para os espelhos de log |
| `status [--clients]` | Estado de cada processo |
| `kill`, `stop`, `start`, `restart <nome>` | Injeção de falha e volta (`kill` é `kill -9`) |
| `client start\|stop\|status` | Sobe e fecha clientes persistentes |
| `client do <nome> <comando...>` | Envia, espera o `[done]` e mostra a saída |
| `client send <nome> <comando...>` / `client wait <nome> <N>` | Envio sem esperar e espera pelo comando `N` |
| `client batch <nome> <arquivo>` | Manda um arquivo de comandos |
| `client attach <nome>` | Digita direto na sessão do cliente |
| `console` | Laço interativo |
| `watch [cenário\|--console]` | Painéis em tmux |
| `run <cenário>` | Executa `scenarios/<cenário>.sh` |
| `exec <host> <cmd...>`, `logs <nome>`, `clean [--data]` | Depuração e limpeza |

## Comandos do cliente

Os mesmos em modo interativo (`bin/client --interactive`), em lote
(`bin/client --batch arquivo`, `-` lê da stdin; `--keep-going` não para no primeiro
erro) e nos arquivos de `batch/`:

```text
upload <arquivo>            download <nome> <saída>
lookup <nome|object-id>     put <nome> <valor...>
state [host porta]          ping|join|leave [host porta]
sleep <segundos>            help | quit
```

Linhas em branco e iniciadas por `#` são ignoradas; aspas duplas agrupam um argumento
com espaços. A sessão tem um NodeID só e escreve estes marcadores na saída:

```text
[ready]            sessão pronta
[done N rc=R]      fim do comando N com código R (é o que o harness espera)
[bye]              sessão fechada
```

Cada cliente roda dentro de `run/<nome>/` na máquina dele, então os arquivos de teste
ficam em `../files/` (por exemplo `upload ../files/documento_a.pdf`).

## Console

```text
cluster> C1 upload ../files/documento_a.pdf     # envia a C1 e mostra a saída e o rc
cluster> C1,C5 state                            # vários clientes de uma vez
cluster> @SP2 kill                              # kill -9 do Super Peer SP2 (também stop, start)
cluster> status
```

Qualquer comando do `cluster.sh` vale no console. Ctrl-D sai sem derrubar o cluster.

## Cenários

| Cenário | O que faz |
| --- | --- |
| `smoke` | Sobe tudo, um upload, um download em outra máquina e confere o `LogIndex` |
| `final` | Roteiro do CP6, uma fase por item da lista do professor (abaixo) |
| `carga` | Todos os clientes ao mesmo tempo: cada um envia um arquivo e baixa o do seguinte. `WAVE=<n>` limita quantos agem por vez |
| `professor` | Encaixe para o conjunto de testes do professor |

Fases do `final`: Bully e Chord no boot, SMR e 2PC (uploads A, B, C), lookup pelo
ObjectID, Gossip e detecção (`kill -9`), 2PC com um participante fora (D, E), IST na
volta, cache LFU, download com `sha256` conferido em duas máquinas, chunks e compressão
(arquivo de 9 MiB) e réplica (derruba o dono de A e baixa de novo).

Ao terminar, `run.sh` grava `tests/cluster/out/relatorio.md` (fases, métricas de tempo e
caminho dos logs) e derruba o cluster. `CLUSTER_KEEP=1` deixa tudo de pé.

## Logs

Cada processo remoto escreve em `run/<nome>/proc.log`. Um espelho por máquina lê todos
esses logs (`ssh tail -F`), carimba cada linha com **o relógio da máquina do operador**
(a spec assume relógios independentes) e grava em `tests/cluster/logs/<nome>.<vida>.log`.
`<nome>.log` aponta para a vida mais recente, e uma nova vida começa quando o processo é
reiniciado (é o `sp5102-rejoin.log` do harness antigo).

## Observações

- O `ssh` da máquina remota pode imprimir lixo antes do comando (`.bashrc`, `motd`). O
  harness descarta tudo até um marcador, então isso não atrapalha.
- Um `upload` feito depois de um `kill -9` e antes da detecção da falha (cerca de 15 s) pode ser
  recusado pelo 2PC. Os cenários esperam o `SUSPECT`/`FAILED` antes de enviar.
- Para ensaiar sem rede, use `host m1 local`: roda no próprio checkout, sem ssh.
