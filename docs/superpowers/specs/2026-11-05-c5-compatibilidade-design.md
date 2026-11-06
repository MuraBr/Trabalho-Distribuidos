# Compatibilidade com o runner original do C5

## Objetivo

Executar as verificações de `tests/c5/run.sh` sobre operações reais de 2PC, cache LFU e recuperação incremental, preservando a separação arquitetural entre Peer e Super Peer.

O usuário escolheu uma camada de compatibilidade em vez de mudar o comportamento padrão dos executáveis.

## Estado observado

- O runner inicia três Super Peers, mas não inicia o Peer exigido pelo upload.
- O lookup por nome do runner não é aceito pelo comando atual de consulta Chord por ObjectID.
- Lookup de metadados não acessa chunks e, portanto, não exercita o LFU.
- O JOIN do cliente legado não inicia um Super Peer atrasado.
- `|| true` oculta os códigos de erro dos comandos.
- O gerador de fixtures escreve em `tests/data`, inclusive por caminho relativo.
- A árvore já contém alterações do usuário em `tests/data/SHA256SUMS` e a remoção de `tests/c5/c5.conf`; devem ser preservadas.

## Alternativas e decisão

1. Alterar o runner original: simplifica a preparação, mas deixa de preservar o teste fornecido.
2. Alterar as semânticas padrão de upload, lookup e JOIN: afeta os contratos existentes e mistura responsabilidades.
3. Adaptadores explícitos via `NODE_BIN` e `CLIENT_BIN`: mantém os executáveis e o teste originais, acrescentando preparação e verificações reais. Alternativa escolhida pelo usuário.

## Contrato de execução proposto

Decisão refinada e aprovada: preparar a sessão e executar o comando original:

```bash
source tests/c5/compat_env.sh
bash ./tests/c5/run.sh
```

Esse comando monta o ambiente de compatibilidade e chama o runner original por meio de uma cópia byte a byte, com os mesmos arquivos auxiliares e configuração em uma área temporária. A cópia permite que o gerador produza fixtures sem sobrescrever os dados do projeto. O runner original e suas asserções não são editados.

Um bootstrap `BASH_ENV`, restrito ao caminho exato do runner original, delega a execução à área temporária e aos adaptadores. Outros comandos Bash não são interceptados. Sem o `source`, o runner mantém seu comportamento original. A camada não deve sobrescrever um `BASH_ENV` preexistente diferente sem autorização.

## Responsabilidades da camada

### Preparação e isolamento

- Compilar os executáveis reais em diretório temporário, com flags estritas.
- Preservar os bytes do runner e da configuração originais e verificar essa igualdade.
- Detectar portas necessárias já ocupadas e falhar com diagnóstico, sem encerrar processos do usuário.
- Usar armazenamento, sockets locais, downloads, fixtures e logs isolados por execução.
- Não alterar os arquivos de dados existentes nem as mudanças locais do usuário.

### Adaptador de nós

- Receber os argumentos que o runner enviaria ao `bin/node`.
- Iniciar os Super Peers reais com a configuração original e dados isolados.
- Aguardar e verificar a inicialização e a convergência, com prazos limitados.
- Supervisionar e encerrar somente os processos iniciados pela execução.

### Adaptador de cliente

- Upload: garantir que um Peer real esteja ativo e registrado, então executar upload com sua identidade pelo canal local existente. Verificar o sucesso e guardar o ObjectID esperado.
- Lookup por nome: realizar a resolução normal e um download real para um destino temporário exclusivo. Os dois lookups do runner passam a exercitar duas leituras reais dos chunks. Verificar o conteúdo baixado e os eventos reais de MISS/HIT no Peer.
- JOIN: executar a ação legada quando aplicável e iniciar adicionalmente um Super Peer real atrasado, conectado ao cluster. Verificar a recuperação do histórico e do catálogo antes de relatar sucesso de IST.
- Identificar nos logs as ações adicionais como preparação/verificação da camada de compatibilidade. Não apresentar os comandos padrão do programa como se tivessem adquirido essas semânticas.

### Evidências e falhas

- Capturar logs reais dos processos, incluindo os eventos de PREPARE/COMMIT e do cache.
- Não gerar PREPARE, COMMIT, cache HIT ou confirmação de IST sem observar ou verificar a operação correspondente.
- Além das buscas por texto do runner, verificar conteúdo dos downloads, convergência do histórico e recuperação do catálogo.
- Registrar e propagar falhas dos adaptadores mesmo quando o runner usa `|| true`.
- O launcher só termina com sucesso se o runner e as verificações adicionais passarem.
- Conservar logs e caminhos das evidências para inspeção após a limpeza dos processos.

## Verificação planejada

1. Reproduzir a incompatibilidade com o runner original, isolado dos dados do usuário.
2. Testar parsing dos comandos, falhas de preparação e limpeza dos processos.
3. Rodar o runner original através da camada e exigir todas as suas asserções.
4. Exigir evidências adicionais de downloads íntegros, MISS seguido de HIT e recuperação efetiva do nó atrasado.
5. Rodar os testes locais de cache e journal e a integração C5 já existente.
6. Confirmar que runner, configuração e mudanças preexistentes do usuário foram preservados.

## Fora do escopo

- Não corrigir a eleição Bully ou implementar garantias gerais sob partições nesta mudança.
- Não modificar o protocolo de produção, os formatos de armazenamento ou a CLI padrão.
- Não declarar a camada como substituta da validação independente dos requisitos do trabalho.

## Documentação

Explicar o novo comando e suas ações adicionais. Atualizar `requisitos_aluno_2.md` e `explicacao_codigo_aluno_2.md` com a distinção entre adaptação de teste e comportamento de produção, conforme `AGENTS.md`. Não modificar `requisitos_aluno_1.md`.
