#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/peer.conf" <<'EOF'
ip=127.0.0.1
port=55102
superpeer-host=127.0.0.1
superpeer-port=55101
EOF

printf '# comentário\nhelp\nsleep "0"\nquit\n' | "$ROOT/bin/client" --interactive --config "$TMP/peer.conf" >"$TMP/session.log" 2>&1
grep -Eq '^NodeID: [[:xdigit:]]{64}$' "$TMP/session.log"
grep -qx '\[ready\]' "$TMP/session.log"
grep -qx '\[done 1 rc=0\]' "$TMP/session.log"
grep -qx '\[done 2 rc=0\]' "$TMP/session.log"
grep -qx '\[done 3 rc=0\]' "$TMP/session.log"
grep -qx '\[bye\]' "$TMP/session.log"

printf 'comando-inexistente\nquit\n' | "$ROOT/bin/client" --interactive --config "$TMP/peer.conf" >"$TMP/error.log" 2>&1
grep -qx '\[done 1 rc=1\]' "$TMP/error.log"
grep -qx '\[bye\]' "$TMP/error.log"

printf 'help\nquit\n' | "$ROOT/bin/client" --batch - --config "$TMP/peer.conf" >"$TMP/batch.log" 2>&1
grep -qx '\[done 1 rc=0\]' "$TMP/batch.log"
grep -qx '\[done 2 rc=0\]' "$TMP/batch.log"

if printf 'comando-inexistente\nhelp\n' | "$ROOT/bin/client" --batch - --config "$TMP/peer.conf" >"$TMP/batch-stop.log" 2>&1; then
    echo 'batch without --keep-going should return nonzero' >&2
    exit 1
fi
grep -qx '\[done 1 rc=1\]' "$TMP/batch-stop.log"
if grep -qx '\[done 2 rc=0\]' "$TMP/batch-stop.log"; then
    echo 'batch without --keep-going continued after an error' >&2
    exit 1
fi

if printf 'comando-inexistente\nhelp\n' | "$ROOT/bin/client" --batch - --keep-going --config "$TMP/peer.conf" >"$TMP/batch-keep-going.log" 2>&1; then
    echo 'batch with a failed command should return nonzero' >&2
    exit 1
fi
grep -qx '\[done 1 rc=1\]' "$TMP/batch-keep-going.log"
grep -qx '\[done 2 rc=0\]' "$TMP/batch-keep-going.log"

mkdir "$TMP/empty-state"
: >"$TMP/empty-state/c4-operation.log"
"$ROOT/bin/client" --cmd state --data-dir "$TMP/empty-state" >"$TMP/state.log" 2>&1
grep -qx 'LogIndex 0' "$TMP/state.log"
grep -qx 'Objects: 0' "$TMP/state.log"

echo 'client session tests: ok'
