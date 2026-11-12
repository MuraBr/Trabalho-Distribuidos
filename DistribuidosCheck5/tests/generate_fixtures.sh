# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
# Ano: 2015


#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"

mkdir -p "$DATA_DIR"

printf 'Checkpoint C2 fixture\nLinha repetida para testar compressão.\n%.0s' {1..1000} \
  > "$DATA_DIR/small.txt"

dd if=/dev/zero of="$DATA_DIR/exact_4MiB.bin" bs=1M count=4 status=none
dd if=/dev/zero of="$DATA_DIR/over_4MiB.bin" bs=1M count=4 status=none
printf 'X' >> "$DATA_DIR/over_4MiB.bin"

# 10 MiB determinísticos
python3 - <<'PY'
from pathlib import Path
p = Path("/tmp/bittorrent-tests-fixture")
data = bytes((i * 31 + 17) % 256 for i in range(10 * 1024 * 1024))
Path("tests/data/random_10MiB.bin").write_bytes(data)
PY

cp "$DATA_DIR/small.txt" "$DATA_DIR/documento_a.pdf"
cp "$DATA_DIR/over_4MiB.bin" "$DATA_DIR/documento_b.pdf"
cp "$DATA_DIR/random_10MiB.bin" "$DATA_DIR/documento_c.pdf"

sha256sum "$DATA_DIR"/* > "$DATA_DIR/SHA256SUMS"
echo "Fixtures geradas em $DATA_DIR"
