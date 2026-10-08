#!/usr/bin/env python3
"""Evidências TCP C3 em diretórios temporários, sem reutilizar dados do usuário."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time
import zlib
from config import load_superpeer_config

parser = argparse.ArgumentParser()
parser.add_argument("--bin-dir", default="bin")
parser.add_argument("--real-time", action="store_true")
parser.add_argument("--config")
args = parser.parse_args()
bins = Path(args.bin_dir).resolve()
root = Path(tempfile.mkdtemp(prefix="pd-c3-failures-"))
processes = []
hosts = {}
configured_nodes = load_superpeer_config(args.config) if args.config else []
if configured_nodes and len(configured_nodes) != 5:
    raise ValueError("O cenário de Gossip C3 precisa de exatamente cinco Super Peers na configuração")
config_by_port = {entry["port"]: entry for entry in configured_nodes}
passed = 0
env = dict(os.environ)
if not args.real_time:
    env.update(C3_HEARTBEAT_MS="500", C3_SUSPECT_MS="3000", C3_FAILED_MS="5000", C3_REMOVED_MS="7000", C3_RPC_MS="300")
suspect, failed, removed = (15, 20, 30) if args.real_time else (3, 5, 7)

def check(value, description):
    global passed
    assert value, description
    passed += 1
    print("PASS", description, flush=True)

def wait(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.1)
    raise AssertionError("Prazo esgotado aguardando condição")

def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def launch(name, command, marker):
    directory = root / name
    directory.mkdir(exist_ok=True)
    log = directory / "process.log"
    old_size = log.stat().st_size if log.exists() else 0
    with log.open("a") as output:
        p = subprocess.Popen([str(bins / command[0]), *map(str, command[1:]), "--data-dir", str(directory)], env=env, cwd=root, stdout=output, stderr=subprocess.STDOUT)
    processes.append(p)
    if "--port" in command:
        port_index = command.index("--port") + 1
        if port_index < len(command):
            entry = config_by_port.get(int(command[port_index]))
            hosts[int(command[port_index])] = entry["host"] if entry else "127.0.0.1"
    # O log pode conter uma inicialização antiga: conferir listener e processo atual também.
    wait(lambda: p.poll() is not None or marker in log.read_text()[old_size:], 15)
    assert p.poll() is None, log.read_text()
    return p, directory, log

def identity(log):
    return bytes.fromhex(re.findall(r"NodeID: ([0-9a-f]{64})", log.read_text())[-1])

def ledger(directory):
    path = directory / "membership.bin"
    if not path.exists():
        return {}
    data = path.read_bytes()
    assert data[:8] == b"PDMEMB1\0"
    n = struct.unpack("!I", data[8:12])[0]
    assert len(data) == 12 + 186 * n
    records = [data[12 + i * 186:12 + (i + 1) * 186] for i in range(n)]
    return {r[:32]: {"state": r[153], "incarnation": struct.unpack("!Q", r[129:137])[0], "role": r[96]} for r in records}

def state(directory, node):
    return ledger(directory).get(node, {}).get("state")

def stop(p, crash=False):
    if p.poll() is None:
        p.send_signal(signal.SIGCONT)
        p.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        p.wait(timeout=12)

def command(local, *arguments, ok=True):
    result = subprocess.run([str(bins / "peer"), "--local-peer-port", str(local), *map(str, arguments)], env=env, cwd=root, text=True, capture_output=True, timeout=25)
    with (root / "commands.log").open("a") as log:
        log.write(result.stdout + result.stderr)
    if ok:
        assert result.returncode == 0, result.stdout + result.stderr
    return result

def exact(s, size):
    output = b""
    while len(output) < size:
        chunk = s.recv(size - len(output))
        assert chunk, "resposta truncada"
        output += chunk
    return output

def request(number, kind, payload=b"", source=bytes(32), destination=bytes(32)):
    tx = os.urandom(16)
    header = struct.pack("!BB32s32s16sQII", 1, kind, source, destination, tx, int(time.time()), len(payload), 0)
    frame = header[:-4] + struct.pack("!I", zlib.crc32(header + payload)) + payload
    with socket.create_connection((hosts.get(number, "127.0.0.1"), number), timeout=2) as s:
        s.settimeout(3)
        s.sendall(frame)
        reply = exact(s, 98)
        fields = struct.unpack("!BB32s32s16sQII", reply)
        body = exact(s, fields[6])
    assert fields[4] == tx and zlib.crc32(reply[:-4] + bytes(4) + body) == fields[7]
    return fields[1], body

def descriptor(number):
    kind, body = request(number, 20)
    assert kind == 20 and len(body) == 64
    digest = hashlib.sha256(socket.inet_pton(socket.AF_INET, body[:46].split(b"\0", 1)[0].decode()) + body[46:]).digest()
    return digest, body

def routed(entry, key, alive):
    visited = []
    number = entry
    for _ in range(12):
        assert number not in visited, "ciclo no lookup"
        visited.append(number)
        kind, body = request(number, 21, key)
        assert kind == 21 and len(body) == 65
        target = struct.unpack("!H", body[47:49])[0]
        ip = body[1:47].split(b"\0", 1)[0].decode()
        digest = hashlib.sha256(socket.inet_pton(socket.AF_INET, ip) + body[47:]).digest()
        assert digest in alive, "lookup referencia nó morto"
        if body[0] == 1:
            expected = next((n for n in sorted(alive) if n >= key), min(alive))
            assert digest == expected
            return
        number = target
    raise AssertionError("lookup não terminou")

try:
    sp_port, a_port, b_port = port(), port(), port()
    sp, sd, sl = launch("sp", ["superpeer", "--port", sp_port], "NodeID:")
    a, ad, al = launch("a", ["peer", "serve", a_port, "127.0.0.1", sp_port], "Peer storage started")
    b, bd, bl = launch("b", ["peer", "serve", b_port, "127.0.0.1", sp_port], "Peer storage started")
    aid, bid, sid = identity(al), identity(bl), identity(sl)
    document = root / "sample.pdf"
    document.write_bytes(b"PDF sintetico C3\n" * 10000)
    digest = hashlib.sha256(document.read_bytes()).hexdigest()
    command(a_port, "upload", document, "127.0.0.1", a_port)
    check(request(sp_port, 13, b"", aid, sid)[0] == 2, "heartbeat malformado rejeitado com CRC e TransactionID válidos")
    check(request(sp_port, 14, b"\1curto", aid, sid)[0] == 2, "Gossip malformado rejeitado")
    # Detector não pode remover as localizações durante a janela SUSPECT.
    a.send_signal(signal.SIGSTOP)
    wait(lambda: state(sd, aid) == 1, suspect + 4)
    check(state(sd, aid) == 1, "Peer pausado fica SUSPECT")
    a.send_signal(signal.SIGCONT)
    wait(lambda: state(sd, aid) == 0, 6)
    target = root / "after-pause.pdf"
    command(b_port, "download", digest, target, "127.0.0.1", sp_port)
    check(target.read_bytes() == document.read_bytes(), "recuperação durante SUSPECT preserva documento")
    reconnects_before_restart = al.read_text().count("Peer reconnected; catalog announced")
    stop(sp, True)
    time.sleep(.6)
    sp, sd, sl = launch("sp", ["superpeer", "--port", sp_port], "NodeID:")
    wait(lambda: al.read_text().count("Peer reconnected; catalog announced") > reconnects_before_restart, 12)
    target = root / "after-sp-restart.pdf"
    wait(lambda: command(b_port, "download", digest, target, "127.0.0.1", sp_port, ok=False).returncode == 0, 15)
    check(target.read_bytes() == document.read_bytes(), "SP reiniciado: Peer ativo refaz JOIN/ANNOUNCE e download")
    stop(a, True)
    wait(lambda: state(sd, aid) == 1, suspect + 5)
    wait(lambda: state(sd, aid) == 2, failed + 5)
    check(state(sd, aid) == 2, "kill -9 Peer confirma FAILED sem LEAVE")
    unavailable = command(b_port, "download", digest, root / "absent.pdf", "127.0.0.1", sp_port, ok=False)
    check(unavailable.returncode != 0, "FAILED retira disponibilidade do diretório")
    wait(lambda: state(sd, aid) == 3, removed + 5)
    old_epoch = ledger(sd)[aid]["incarnation"]
    a, ad, al = launch("a", ["peer", "serve", a_port, "127.0.0.1", sp_port], "Peer storage started")
    wait(lambda: state(sd, aid) == 0, 10)
    check(identity(al) == aid and ledger(sd)[aid]["incarnation"] > old_epoch, "reentrada mantém NodeID e incrementa incarnation")
    target = root / "after-peer-restart.pdf"
    command(b_port, "download", digest, target, "127.0.0.1", sp_port)
    check(target.read_bytes() == document.read_bytes(), "manifest e chunks sobrevivem à queda")
    stop(a)
    wait(lambda: state(sd, aid) == 3, 5)
    check(state(sd, aid) == 3, "LEAVE voluntário cria REMOVED diretamente")
    stop(b); stop(sp)

    # Cinco processos: Gossip deve disseminar conhecimento além dos vizinhos Chord.
    overlay = []
    bootstrap = configured_nodes[0]["port"] if configured_nodes else port()
    bootstrap_host = configured_nodes[0]["host"] if configured_nodes else "127.0.0.1"
    for i in range(5):
        entry = configured_nodes[i] if configured_nodes else None
        number = entry["port"] if entry else bootstrap if i == 0 else port()
        options = ["superpeer", "--port", number]
        if entry:
            options += ["--config", str(Path(args.config).resolve())]
        if i and not entry:
            options += ["--chord-host", bootstrap_host, "--chord-port", bootstrap]
        p, directory, log = launch(f"ring{i}", options, "NodeID:")
        overlay.append((p, directory, log, number, identity(log)))
        time.sleep(.4)
    ids = {entry[4] for entry in overlay}
    wait(lambda: all(all(state(d, node) == 0 and ledger(d)[node]["incarnation"] > 0 for node in ids) for _, d, _, _, _ in overlay), 25)
    check(True, "Gossip converge membership dos cinco Super Peers")
    def initial_ring_ready():
        try:
            for _, _, _, number, _ in overlay:
                routed(number, bytes(32), ids)
                routed(number, bytes([255]) * 32, ids)
            return True
        except (AssertionError, OSError):
            return False
    wait(initial_ring_ready, 25)
    # Integra as duas partes: transferência C2 enquanto os Super Peers mantêm o overlay C3.
    storage_port = port()
    storage, storage_directory, storage_log = launch("ring-storage", ["peer", "serve", storage_port, "127.0.0.1", overlay[0][3]], "Peer storage started")
    command(storage_port, "upload", document, "127.0.0.1", storage_port)
    for _, _, _, number, _ in overlay:
        routed(number, bytes.fromhex(digest), ids)
    check(True, "ObjectID real de upload localizado pelo Chord em todos os Super Peers")
    # Mata o sucessor de ring0, não um nó arbitrário.
    successor = sorted(ids)[(sorted(ids).index(overlay[0][4]) + 1) % 5]
    victim = next(entry for entry in overlay if entry[4] == successor)
    stop(victim[0], True)
    survivors = [entry for entry in overlay if entry[4] != successor]
    wait(lambda: all(state(d, successor) in (2, 3) for _, d, _, _, _ in survivors), failed + 12)
    alive = ids - {successor}
    def repaired():
        try:
            for _, _, _, number, _ in survivors:
                for key in [bytes(32), bytes([255]) * 32, *alive]:
                    routed(number, key, alive)
                for finger in range(256):
                    kind, body = request(number, 24, bytes([finger]))
                    assert kind == 24
                    digest = hashlib.sha256(socket.inet_pton(socket.AF_INET, body[:46].split(b"\0", 1)[0].decode()) + body[46:]).digest()
                    assert digest != successor
            return True
        except (AssertionError, OSError):
            return False
    wait(repaired, 25)
    check(True, "falha de sucessor: lookup correto e 256 fingers sem nó morto")
    transfer_after_failure = root / "ring-after-failure.pdf"
    command(storage_port, "download", digest, transfer_after_failure, "127.0.0.1", overlay[0][3])
    check(transfer_after_failure.read_bytes() == document.read_bytes(), "upload/download seguem íntegros durante reparo do overlay")
    # O retorno deve manter NodeID, incrementar época e desfazer o bloqueio do anel.
    victim_epoch = ledger(survivors[0][1])[successor]["incarnation"]
    returning_entry = config_by_port.get(victim[3])
    returning_options = ["superpeer", "--port", victim[3]]
    if returning_entry:
        returning_options += ["--config", str(Path(args.config).resolve())]
    if not returning_entry:
        returning_options += ["--chord-host", hosts.get(survivors[0][3], "127.0.0.1"), "--chord-port", survivors[0][3]]
    returning, returning_directory, returning_log = launch(victim[1].name, returning_options, "NodeID:")
    wait(lambda: all(state(d, successor) == 0 and ledger(d)[successor]["incarnation"] > victim_epoch for _, d, _, _, _ in survivors), 25)
    check(identity(returning_log) == successor, "Super Peer reentra com mesmo NodeID e incarnação maior")
    def rejoined():
        try:
            for _, _, _, number, _ in survivors + [(returning, returning_directory, returning_log, victim[3], successor)]:
                for key in [bytes(32), bytes([255]) * 32, *ids, bytes.fromhex(digest)]:
                    routed(number, key, ids)
                kind, body = request(number, 24, bytes([0]))
                assert kind == 24
                expected = sorted(ids)[(sorted(ids).index(descriptor(number)[0]) + 1) % len(ids)]
                actual = hashlib.sha256(socket.inet_pton(socket.AF_INET, body[:46].split(b"\0", 1)[0].decode()) + body[46:]).digest()
                assert actual == expected
            return True
        except (AssertionError, OSError):
            return False
    wait(rejoined, 25)
    check(True, "membership recuperado libera Chord e recompõe o anel de cinco nós")
finally:
    for p in processes:
        stop(p)
    print("Evidências:", root, flush=True)

diagnostics = re.compile(r"WARNING: ThreadSanitizer|ERROR: AddressSanitizer|LeakSanitizer|runtime error:")
check(all(not diagnostics.search(path.read_text()) for path in root.glob("*/process.log")), "logs sem diagnósticos dos sanitizadores após encerramento")
print(f"RESULTADO C3: {passed} verificações aprovadas", flush=True)
