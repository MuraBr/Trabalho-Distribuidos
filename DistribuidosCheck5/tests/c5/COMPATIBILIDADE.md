# Executar o script original do professor — C5

Na raiz do projeto, no Bash do Ubuntu/WSL:

```bash
source tests/c5/compat_env.sh
bash ./tests/c5/run.sh
```

Não execute os testes C5 simultaneamente: utilizam as portas 5101, 5102 e 5103. Se estiverem ocupadas, a camada encerra com diagnóstico; não mata processos de outra execução. São necessários Python 3, make, compilador C e as bibliotecas já utilizadas pelo projeto.

Para desativar nesta sessão:

```bash
unset BASH_ENV C5_COMPAT_ROOT
```

Fechar o terminal também descarta a preparação. Nenhum arquivo de perfil é modificado. Se já houver outro `BASH_ENV`, a preparação o preserva e informa o conflito.

## O que o source prepara

O Bash lê `BASH_ENV` ao iniciar scripts não interativos. Nosso bootstrap só adapta o caminho exato `tests/c5/run.sh` deste projeto. `bash -c`, outros scripts e os executáveis normais continuam com seu comportamento habitual.

Ao executar o comando original, o controlador:

1. Cria uma área privada `/tmp/pd-c5-compat-*`.
2. Copia o runner, common/env, configuração e gerador de fixtures **sem alterar seus bytes**. Somente a cópia temporária do gerador recebe permissão de execução.
3. Compila os executáveis reais com `-Werror`, sem sobrescrever `bin` do projeto.
4. Define `NODE_BIN` e `CLIENT_BIN` para adaptadores e executa a cópia idêntica do runner no ambiente temporário. Assim, as fixtures geradas não sobrescrevem `tests/data` do projeto.
5. Confere os resultados adicionais, retorna erro se algum adaptador falhar e encerra o grupo de processos criado para esse teste.

Não é uma alteração no arquivo fornecido pelo professor. É uma execução adaptada, não uma alegação de que o runner sem preparação passou. O caminho da área temporária é impresso e suas evidências permanecem disponíveis após o teste.

## Operações reais acrescentadas pelos adaptadores

- O adaptador de nós inicia os três Super Peers reais, mantendo logs individuais e encaminhando suas saídas para o log do runner.
- Antes do upload, inicia um Peer real e espera a liderança convergir. A transferência usa o canal local existente e a identidade desse Peer.
- Os dois comandos de lookup por nome fazem a consulta e, adicionalmente, um download real para destinos temporários distintos. Os bytes são comparados com o original e os eventos MISS/HIT são conferidos no log real do Peer. As linhas de cache encaminhadas ao log compartilhado são cópias dessas linhas reais.
- O JOIN legado é executado e inicia-se adicionalmente um quarto Super Peer atrasado. O controlador escolhe uma identidade de teste inferior às dos ativos, para testar catch-up sem introduzir outra disputa de prioridade. Isso não valida o retorno de um antigo coordenador de prioridade superior.
- Confere o histórico recuperado byte a byte e uma requisição `M_IST_REQUEST` realmente originada pelo nó atrasado.
- O diretório atual só resolve owners registrados localmente. Por isso, depois da recuperação, o adaptador cadastra explicitamente no nó atrasado o endpoint do Peer real, com UUID, NodeID e incarnação atuais, por um JOIN TCP. Essa sonda é identificada no log; não é uma nova identidade nem um novo ANNOUNCE. Não se atribui essa etapa ao IST ou a um failover automático do Peer.
- Um download pelo catálogo do nó atrasado confirma que os metadados recuperados permitem localizar o conteúdo após esse cadastro. Nenhum chunk é copiado para o Super Peer.

PREPARE e COMMIT vêm dos processos reais. O script original usa `|| true`, então a camada conserva falhas de adaptadores e reprova a execução mesmo que as buscas textuais tenham produzido cinco PASS. A mensagem final da camada também deve indicar aprovação.

## Onde conferir

Na área temporária impressa:

- `logs/c5.log`: log compartilhado do runner.
- `sp-5101.log`, `sp-5102.log`, `sp-5103.log`: saídas individuais dos Super Peers.
- `peer.log`: eventos reais LFU.
- `late.log`: nó atrasado.
- `downloads/`: conteúdos usados na comparação.
- `upload.json`, `downloads.json`, `ist.json`: resultados verificados.
- `error-*.txt`: falhas que o runner poderia ter ocultado.

## Verificação da própria camada

```bash
python3 tests/c5/test_compat.py
C5_COMPAT_E2E=1 python3 tests/c5/test_compat.py
```

O segundo comando executa processos e compila novamente. Também verifica preservação dos arquivos originais e liberação das portas. Para os testes independentes C5 já existentes, execute separadamente:

```bash
bash tests/c5/run_integrated.sh
```

## Evidências desta implementação

Na execução da implementação, o runner adaptado concluiu `PASS: 5`, `FAIL: 0`, com aprovação adicional de conteúdo, cache e recuperação. Os seis testes da camada, incluindo integração real e detecção de saída inesperada de nó, passaram. Cache LFU, journal e integração TCP de `make test-c5` passaram em execução separada. Uma tentativa de executar simultaneamente as duas suítes interferiu nas portas compartilhadas e falhou; a repetição sequencial passou. Não executar em paralelo.

Esta camada não corrige os limites de eleição, 2PC sob partições ou concorrência do código C. O resultado comprova os cenários exercitados, não todas as garantias do enunciado.
