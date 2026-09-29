#!/usr/bin/env python3
"""Testes independentes C1/C2; logs e artefatos ficam em diretório temporário informado."""
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
import threading
import ctypes

parser = argparse.ArgumentParser()
parser.add_argument("--bin-dir", default="bin")
args = parser.parse_args()
bins = Path(args.bin_dir).resolve()
root = Path(tempfile.mkdtemp(prefix="pd-c2-integration-"))
processes = []
logs = {}
env = dict(os.environ, PEER_IO_TIMEOUT="2", PEER_CONNECT_TIMEOUT="1", PEER_TRANSFER_THREADS="4")
passed = 0

def check(condition, description):
    global passed
    if not condition:
        raise AssertionError(description)
    passed += 1
    print(f"PASS {description}", flush=True)

def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

sp_port, a_port, b_port = port(), port(), port()

def launch(name, arguments, marker):
    log = root / (name + ".log")
    handle = log.open("w")
    process = subprocess.Popen([str(bins / arguments[0]), *map(str, arguments[1:])], cwd=root, stdout=handle, stderr=subprocess.STDOUT, env=env)
    handle.close()
    processes.append(process)
    logs[name] = log
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"{name} encerrou: {log.read_text()}")
        if marker in log.read_text():
            return process
        time.sleep(.05)
    raise AssertionError(f"{name} não iniciou: {log.read_text()}")

def start_sp(name="sp"):
    return launch(name, ["superpeer", "--port", sp_port, "--name", "integration"], "NodeID:")

def start_peer(name, number):
    return launch(name, ["peer", "serve", number, "127.0.0.1", sp_port], "Peer storage started")

def stop(process, crash=False):
    if process.poll() is None:
        process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        process.wait(timeout=15)

def command(local, *arguments, ok=True):
    result = subprocess.run([str(bins / "peer"), "--local-peer-port", str(local), *map(str, arguments)], cwd=root, text=True, capture_output=True, env=env, timeout=60)
    with (root / "commands.log").open("a") as log:
        log.write(repr(arguments) + "\n" + result.stdout + result.stderr)
    if ok and result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    if not ok and result.returncode == 0:
        raise AssertionError("Comando inválido foi aceito")
    return result.stdout + result.stderr

def node_id(name):
    return re.search(r"NodeID: ([0-9a-f]{64})", logs[name].read_text()).group(1)

def exact(s, size):
    data = b""
    while len(data) < size:
        part = s.recv(size - len(data))
        if not part:
            raise EOFError("Frame truncado")
        data += part
    return data

def frame(kind, payload=b"PING", source=bytes(32), dest=bytes(32), version=1, tx=None):
    tx = tx or os.urandom(16)
    header = struct.pack("!BB32s32s16sQII", version, kind, source, dest, tx, int(time.time()), len(payload), 0)
    crc = zlib.crc32(header + payload)
    return header[:-4] + struct.pack("!I", crc) + payload

def reply(s):
    header = exact(s, 98)
    fields = struct.unpack("!BB32s32s16sQII", header)
    payload = exact(s, fields[6])
    assert zlib.crc32(header[:-4] + bytes(4) + payload) == fields[7]
    return fields, payload

try:
    sp = start_sp()
    a = start_peer("a", a_port)
    b = start_peer("b", b_port)
    aid, bid = node_id("a"), node_id("b")
    check(aid != bid, "identidades distintas")
    samples = []
    for i, size in enumerate([62000, 4194303, 4194304, 4194305, 10485760]):
        path = root / f"sample-{i}.PDF"
        # Conteúdo sintético sem assinatura, como as fixtures do professor.
        path.write_bytes(os.urandom(size) if i == 4 else (bytes(range(256)) * ((size + 255) // 256))[:size])
        samples.append(path)
        output = command(a_port, "upload", path, "127.0.0.1", a_port)
        if i == 4:
            active = peak = 0
            for line in output.splitlines():
                if line.startswith("Worker start"): active += 1
                if line.startswith("Worker finish"): active -= 1
                peak = max(peak, active)
            check(peak > 1 and active == 0, "workers realmente sobrepostos durante upload")
        expected_id = hashlib.sha256(path.read_bytes()).hexdigest()
        check(f"ObjectID: {expected_id}" in output and f"Chunks: {(size + 4194303) // 4194304}\n" in output, f"upload e chunking {size} bytes")
        target = root / f"download-{i}.pdf"
        output = command(b_port, "download", expected_id if i % 2 else path.name, target, "127.0.0.1", sp_port)
        check(target.read_bytes() == path.read_bytes() and "SHA-256 verified" in output, f"download Peer B {size} bytes")
    duplicate = subprocess.run([str(bins / "peer"), "serve", str(a_port), "127.0.0.1", str(sp_port)], cwd=root, capture_output=True, text=True, env=env, timeout=10)
    check(duplicate.returncode != 0, "falha de bind rejeita segunda instância")
    command(b_port, "download", samples[0].name, root / "after-bind-failure.pdf", "127.0.0.1", sp_port)
    check((root / "after-bind-failure.pdf").exists(), "falha de bind não remove cadastro do Peer ativo")
    before_fds = len(list(Path(f"/proc/{b.pid}/fd").iterdir()))
    for i in range(10):
        command(b_port, "download", samples[0].name, root / f"fd-{i}.pdf", "127.0.0.1", sp_port)
    time.sleep(.1)
    check(len(list(Path(f"/proc/{b.pid}/fd").iterdir())) <= before_fds + 1, "descritores estáveis após dez operações")
    config_port = port()
    conf = root / "peer.conf"
    conf.write_text(f"ip=127.0.0.2\nport=1\ndata-dir={root / 'configured-storage'}\nsuperpeer-host=127.0.0.1\nsuperpeer-port={sp_port}\n")
    configured = launch("configured", ["peer", "serve", "--config", conf, "--bind", "127.0.0.2", "--port", config_port], "Peer storage started")
    configured_file = root / "configured.pdf"
    configured_file.write_bytes(b"config-file-content")
    command(b_port, "upload", configured_file, "127.0.0.2", config_port)
    result = command(b_port, "download", configured_file.name, root / "configured-download.pdf", "127.0.0.1", sp_port)
    check(f"127.0.0.2:{config_port}" in result and (root / "configured-storage" / "node.uuid").exists(), "configuração lida, IP anunciado e precedência CLI")
    stop(configured)
    check(f"tipo=6, origem={bid}" in logs["sp"].read_text(), "LOOKUP usa NodeID do Peer B")
    check(f"tipo=8 origem={bid}" in logs["a"].read_text(), "DOWNLOAD_REQ usa NodeID do Peer B")
    check("origem=" + "0" * 64 not in logs["sp"].read_text(), "operações C2 sem origem anônima")

    for count in [1, 2]:
        with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
            request = frame(3)
            if count == 1:
                for index in range(0, len(request), 7): s.sendall(request[index:index+7])
            else:
                s.sendall(request * 2)
            for _ in range(count):
                fields, payload = reply(s)
                check(fields[1] == 4 and payload == b"PONG" and fields[4] == request[66:82], "framing independente e TransactionID preservado")
    oversized = struct.pack("!BB32s32s16sQII", 1, 3, bytes(32), bytes(32), os.urandom(16), 0, 5 * 1024 * 1024 + 1, 0)
    for invalid in [frame(3, version=99), frame(3)[:-1] + b"X", frame(3)[:40], oversized]:
        with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
            s.sendall(invalid)
            s.shutdown(socket.SHUT_WR)
            try: closed = s.recv(1) == b""
            except ConnectionResetError: closed = True
            check(closed, "frame inválido rejeitado por fechamento, não timeout")
    with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
        s.sendall(frame(6, b"bad"))
        fields, data = reply(s)
        check(fields[1] == 2 and data == bytes([1, 6]), "LOOKUP sem identidade rejeitado especificamente")
    with socket.create_connection(("127.0.0.1", sp_port), timeout=5) as s:
        s.sendall(frame(13, b""))
        fields, data = reply(s)
        check(fields[1] == 2 and data == bytes([1, 5]), "heartbeat permanece não implementado")

    # Serialização independente e corrupção em requests reais ao Peer.
    def exchange(payload):
        with socket.create_connection(("127.0.0.1", a_port), timeout=5) as connection:
            connection.sendall(frame(7, payload, bytes.fromhex(bid), bytes.fromhex(aid)))
            return reply(connection)
    raw = b"abc"
    oid = hashlib.sha256(raw).digest()
    def begin(name, object_id=oid):
        encoded = name.encode()
        return bytes([1]) + object_id + struct.pack("!QQBH", 3, 1, 1, len(encoded)) + encoded
    fields, payload = exchange(begin("invalid.txt"))
    check(fields[1] == 2 and payload == bytes([1, 4]), "receptor rejeita extensão não PDF")
    fields, _ = exchange(begin("wire.pdf"))
    check(fields[1] == 1, "BEGIN independente aceito")
    lz4 = ctypes.CDLL("liblz4.so.1")
    lz4.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lz4.LZ4_compress_default.restype = ctypes.c_int
    compressed = ctypes.create_string_buffer(32)
    size = lz4.LZ4_compress_default(raw, compressed, len(raw), 32)
    body = compressed.raw[:size]
    def chunk(object_id=oid, index=0, offset=0, digest=oid):
        return bytes([2]) + object_id + struct.pack("!QQII", index, offset, len(raw), len(body)) + digest + body
    for malformed in [chunk(digest=bytes(32)), chunk(offset=1), chunk(index=1)]:
        fields, payload = exchange(malformed)
        check(fields[1] == 2 and payload == bytes([1, 4]), "chunk malformado rejeitado na rede")
    with socket.create_connection(("127.0.0.1", a_port), timeout=5) as connection:
        request = frame(7, chunk(), bytes.fromhex(bid), bytes.fromhex(aid))
        connection.sendall(request[:-2])
        connection.shutdown(socket.SHUT_WR)
        check(connection.recv(1) == b"", "desconexão no meio do chunk não recebe ACK")
    fields, _ = exchange(chunk())
    check(fields[1] == 1, "chunk reenviado após desconexão")
    fields, _ = exchange(bytes([3]) + oid)
    check(fields[1] == 1, "COMMIT após retomada publica documento")
    wrong_id = hashlib.sha256(b"xyz").digest()
    exchange(begin("wrong-id.pdf", wrong_id))
    exchange(chunk(object_id=wrong_id))
    fields, payload = exchange(bytes([3]) + wrong_id)
    check(fields[1] == 2 and payload == bytes([1, 4]), "ObjectID final divergente rejeitado na rede")

    bad = root / "not-pdf.txt"
    bad.write_text("existe mas não é PDF")
    output = command(a_port, "upload", bad, "127.0.0.1", a_port, ok=False)
    check("Invalid argument" in output, "extensão rejeitada, sem falso positivo ENOENT")
    existing = root / "download-0.pdf"
    old = existing.read_bytes()
    output = command(b_port, "download", samples[0].name, existing, "127.0.0.1", sp_port, ok=False)
    check("File exists" in output and existing.read_bytes() == old, "destino existente preservado")
    for n in ["one", "two"]:
        folder = root / n
        folder.mkdir()
        path = folder / "ambiguous.pdf"
        path.write_text(n)
        command(a_port, "upload", path, "127.0.0.1", a_port)
    output = command(b_port, "download", "ambiguous.pdf", root / "ambiguous-result.pdf", "127.0.0.1", sp_port, ok=False)
    check("Name not unique" in output, "ambiguidade identificada pelo erro correto")

    command(b_port, "upload", samples[0], "127.0.0.1", b_port)
    stop(a, crash=True)  # Mantém localização obsoleta, sem LEAVE.
    output = command(b_port, "download", samples[0].name, root / "fallback.pdf", "127.0.0.1", sp_port)
    check(f"attempt 1: 127.0.0.1:{a_port}" in output and f"attempt 2: 127.0.0.1:{b_port}" in output, "fallback realmente tentou o primeiro Peer")
    stalled = socket.socket()
    stalled.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    stalled.bind(("127.0.0.1", a_port))
    stalled.listen()
    done = threading.Event()
    def stall():
        connection, _ = stalled.accept()
        with connection:
            done.wait(10)
    thread = threading.Thread(target=stall)
    thread.start()
    started = time.monotonic()
    try:
        output = command(b_port, "download", samples[0].name, root / "stalled-fallback.pdf", "127.0.0.1", sp_port)
        elapsed = time.monotonic() - started
        check(1.5 <= elapsed < 8 and "attempt 2:" in output, "timeout libera worker para próximo Peer")
    finally:
        done.set()
        thread.join(timeout=12)
        stalled.close()
    stop(b)
    stop(sp)
    sp = start_sp("sp-restart")
    a = start_peer("a-restart", a_port)
    b = start_peer("b-restart", b_port)
    check(node_id("a-restart") == aid and node_id("b-restart") == bid, "UUID/NodeID persistem")
    output = command(b_port, "download", samples[-1].name, root / "after-restart.pdf", "127.0.0.1", sp_port)
    check((root / "after-restart.pdf").read_bytes() == samples[-1].read_bytes(), "índice reconstruído após reinício também do Super Peer")
    check("tipo=7" in logs["sp-restart"].read_text(), "novo ANNOUNCE comprovado")
    for process in reversed(processes):
        if process.poll() is None: stop(process)
    check(not any(re.search(r"WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:|LeakSanitizer", log.read_text()) for log in logs.values()), "logs sem diagnósticos dos sanitizadores")
    print(f"RESULTADO: {passed} verificações aprovadas", flush=True)
finally:
    for process in reversed(processes):
        if process.poll() is None:
            try: stop(process)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
    print(f"Evidências: {root}", flush=True)
