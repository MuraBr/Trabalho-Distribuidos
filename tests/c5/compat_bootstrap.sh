#!/usr/bin/env bash
# BASH_ENV só adapta o runner escolhido, nunca comandos Bash genéricos.
if [[ -n "${C5_COMPAT_ROOT:-}" && "$0" == *run.sh ]]; then
    if [[ "$(realpath -- "$0" 2>/dev/null)" == "$C5_COMPAT_ROOT/tests/c5/run.sh" ]]; then
        exec python3 "$C5_COMPAT_ROOT/tests/c5/compat.py" run "$@"
    fi
fi
