#!/usr/bin/env python3
"""Verifica entrada e roteamento Chord entre tres processos Super Peer."""
import hashlib
import argparse
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

root = Path(tempfile.mkdtemp(prefix="pd-chord-"))
parser = argparse.ArgumentParser()
parser.add_argument("--bin-dir", default=str(Path(__file__).resolve().parents[2] / "bin"))
parser.add_argument("--config")
args = parser.parse_args()
config_path = str(Path(args.config).resolve()) if args.config else None
binary = Path(args.bin_dir).resolve() / "superpeer"
processes = []
hosts = {}

def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]

def start(port, bootstrap=None, entry=None, bootstrap_host="127.0.0.1"):
    args = [str(binary), "--port", str(port)]
    args += args_for_config()
    host = entry["host"] if entry else "127.0.0.1"
    if bootstrap is not None and not config_path:
        args += ["--chord-host", bootstrap_host, "--chord-port", str(bootstrap)]
    path = root / f"{port}.log"
    with path.open("w") as log:
        process = subprocess.Popen(args, cwd=root, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, PEER_IO_TIMEOUT="2", PEER_CONNECT_TIMEOUT="1"))
    processes.append(process)
    hosts[port] = host
    for _ in range(100):
        content = path.read_text()
        if process.poll() is not None:
            raise AssertionError(f"Super Peer {port} encerrou: {content}")
        found = re.search(r"NodeID: ([0-9a-f]{64})", content)
        if found:
            return bytes.fromhex(found.group(1))
        time.sleep(.05)
    raise AssertionError(f"Super Peer {port} nao iniciou: {path.read_text()}")

def args_for_config():
    return ["--config", config_path] if config_path else []

def client_command(command, host, port, *options):
    client = binary.parent / "client"
    result = subprocess.run([str(client), "--cmd", command, "--host", host, "--port", str(port), *options], cwd=root, text=True, capture_output=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout

def exact(sock, size):
    result = b""
    while len(result) < size:
        piece = sock.recv(size - len(result))
        if not piece:
            raise AssertionError("Resposta truncada")
        result += piece
    return result

def request(port, kind, payload=b""):
    transaction = os.urandom(16)
    header = struct.pack("!BB32s32s16sQII", 1, kind, bytes(32), bytes(32), transaction, int(time.time()), len(payload), 0)
    frame = header[:-4] + struct.pack("!I", zlib.crc32(header + payload)) + payload
    with socket.create_connection((hosts.get(port, "127.0.0.1"), port), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(frame)
        received = exact(sock, 98)
        fields = struct.unpack("!BB32s32s16sQII", received)
        body = exact(sock, fields[6])
    assert fields[4] == transaction and zlib.crc32(received[:-4] + bytes(4) + body) == fields[7]
    return fields[1], body

def descriptor(data):
    assert len(data) == 64
    ip = data[:46].split(bytes(1), 1)[0].decode("ascii")
    port = struct.unpack("!H", data[46:48])[0]
    digest = hashlib.sha256(socket.inet_pton(socket.AF_INET, ip) + data[46:48] + data[48:64]).digest()
    return digest, port

def lookup(entry, key):
    visited = set()
    port = entry
    for _ in range(8):
        assert port not in visited, "Ciclo no roteamento"
        visited.add(port)
        kind, payload = request(port, 21, key)
        assert kind == 21 and len(payload) == 65
        digest, port = descriptor(payload[1:])
        if payload[0] == 1:
            return digest
    raise AssertionError("Lookup nao terminou")

try:
    entries = load_superpeer_config(args.config) if args.config else []
    if entries and len(entries) < 3:
        raise ValueError("A configuração C3 precisa de pelo menos três Super Peers")
    selected = entries[:3]
    ports = [entry["port"] for entry in selected] if selected else [free_port() for _ in range(3)]
    bootstrap_host = selected[0]["host"] if selected else "127.0.0.1"
    ids = [start(ports[0], entry=selected[0] if selected else None), start(ports[1], ports[0], selected[1] if selected else None, bootstrap_host), start(ports[2], ports[0], selected[2] if selected else None, bootstrap_host)]
    sorted_ids = sorted(ids)
    deadline = time.monotonic() + 20
    while True:
        try:
            for index, node_id in enumerate(ids):
                expected = sorted_ids[(sorted_ids.index(node_id) + 1) % 3]
                kind, body = request(ports[index], 24, bytes([0]))
                assert kind == 24 and descriptor(body)[0] == expected
                previous = sorted_ids[(sorted_ids.index(node_id) - 1) % 3]
                kind, body = request(ports[index], 22)
                assert kind == 22 and descriptor(body)[0] == previous
                for key in ids + [bytes(32), bytes([255]) * 32]:
                    owner = next((candidate for candidate in sorted_ids if candidate >= key), sorted_ids[0])
                    assert lookup(ports[index], key) == owner
            break
        except (AssertionError, OSError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)
    topology = client_command("topology", bootstrap_host, ports[0])
    assert "Successor:" in topology and "Finger" in topology, topology
    lookup_output = client_command("lookup", bootstrap_host, ports[0], "--object-id", "ab" * 32)
    assert "Lookup" in lookup_output and "Owner:" in lookup_output, lookup_output
    # O laco de manutencao atualiza 16 entradas por segundo; a ultima requer ate 16 ciclos.
    deadline = time.monotonic() + 22
    while True:
        try:
            for index, node_id in enumerate(ids):
                finger_start = ((int.from_bytes(node_id, "big") + (1 << 255)) % (1 << 256)).to_bytes(32, "big")
                expected = next((candidate for candidate in sorted_ids if candidate >= finger_start), sorted_ids[0])
                kind, body = request(ports[index], 24, bytes([255]))
                assert kind == 24 and descriptor(body)[0] == expected
            break
        except (AssertionError, OSError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)
    kind, _ = request(ports[0], 23, bytes(64))
    assert kind == 2, "NOTIFY sem identidade valida foi aceito"
    kind, _ = request(ports[0], 21, b"curto")
    assert kind == 2, "ROUTE malformado foi aceito"
    print("Chord TCP: tres Super Peers, successor, predecessor, finger[0/255], lookup circular e mensagens invalidas OK")
finally:
    for process in processes:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
