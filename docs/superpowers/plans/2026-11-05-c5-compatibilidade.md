# Plano — compatibilidade C5

**Objetivo:** permitir `source tests/c5/compat_env.sh` seguido de `bash ./tests/c5/run.sh`, sem modificar o runner e sem evidências artificiais.

**Arquitetura:** bootstrap Bash restrito ao runner; controlador Python com processos reais, cópia temporária idêntica do teste, adaptadores NODE_BIN/CLIENT_BIN e verificações adicionais. Não altera os executáveis C.

**Especificação:** `docs/superpowers/specs/2026-11-05-c5-compatibilidade-design.md`, com o refinamento aprovado na conversa.

## Tarefas

- [x] Escrever testes da interface source/bash, isolamento e propagação de falhas; observar RED pela ausência da camada.
- [x] Implementar bootstrap e controlador; testar setup, portas ocupadas e shell não afetado.
- [x] Implementar adaptadores reais de nó/upload/download/JOIN e evidências de cache e IST.
- [x] Executar runner original e suítes locais/integradas C5 em builds temporários.
- [x] Revisar diff, confirmar preservação dos arquivos preexistentes e documentar comandos, limites e resultados reais.

Resultado: runner adaptado 5/5 e seis testes da camada aprovados; suíte C5 existente aprovada separadamente. Revisão independente de padrões e especificação encontrou falha de TIME_WAIT e saída inesperada de nó; ambas corrigidas e verificadas. Nenhuma alteração nos arquivos C ou nos testes originais.

## Foco de revisão

Portas ocupadas; `BASH_ENV` preexistente; erro ocultado por `|| true`; interrupção com filhos vivos; fixtures e logs fora da área temporária. Testes devem cobrir rejeição de conflito, status final não zero, limpeza e igualdade byte a byte dos scripts copiados.

O usuário autorizou implementação nesta sessão. Não fazer commit nem alterar dados existentes.
