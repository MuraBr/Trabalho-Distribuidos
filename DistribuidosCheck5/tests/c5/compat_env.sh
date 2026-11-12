#!/usr/bin/env bash
# Preparação explícita; nenhuma alteração permanente no perfil do usuário.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo 'Use: source tests/c5/compat_env.sh' >&2
    exit 1
fi
_c5_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
_c5_hook="$_c5_root/tests/c5/compat_bootstrap.sh"
if [[ -n "${BASH_ENV:-}" && "$BASH_ENV" != "$_c5_hook" ]]; then
    echo 'BASH_ENV já está definido; não será sobrescrito. Use outra sessão sem essa configuração.' >&2
    unset _c5_root _c5_hook
    return 1
fi
if ! command -v python3 >/dev/null; then
    echo 'A camada C5 requer python3.' >&2
    unset _c5_root _c5_hook
    return 1
fi
export C5_COMPAT_ROOT="$_c5_root"
export BASH_ENV="$_c5_hook"
unset _c5_root _c5_hook
printf '%s\n' 'Compatibilidade C5 preparada. Execute: bash ./tests/c5/run.sh' 'Para desativar: unset BASH_ENV C5_COMPAT_ROOT'
