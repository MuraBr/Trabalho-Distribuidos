#!/usr/bin/env python3
"""Verifica ANNOUNCE em 2PC entre os Super Peers ativos via TCP."""
import argparse
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "c3"))
from config import load_superpeer_config

parser = argparse.ArgumentParser()
parser.add_argument("--bin-dir", default="bin")
parser.add_argument("--config", required=True)
args = parser.parse_args()
bins = Path(args.bin_dir).resolve()
entries = load_superpeer_config(args.config)
root = Path(tempfile.mkdtemp(prefix="pd-c5-2pc-"))
env = dict(os.environ, PEER_IO_TIMEOUT="2", PEER_CONNECT_TIMEOUT="1", PEER_CACHE_BYTES="1048576", C3_HEARTBEAT_MS="250", C3_SUSPECT_MS="900", C3_FAILED_MS="1800", C3_REMOVED_MS="5000", C3_RPC_MS="500")
processes = []
logs = {}

def launch(name, command, marker, timeout=20):
    path = root / f"{name}.log"
    stream = path.open("w")
    process = subprocess.Popen(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, env=env)
    stream.close()
    processes.append(process)
    logs[name] = path
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"{name} encerrou: {path.read_text()}")
        if marker in path.read_text(errors="replace"):
            return process
        time.sleep(.05)
    raise AssertionError(f"{name} não iniciou: {path.read_text(errors='replace')}")

def wait_for(predicate, description, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.1)
    raise AssertionError(f"Timeout: {description}\n" + "\n".join(p.read_text(errors="replace") for p in logs.values()))

def exact(sock, size):
    data = b""
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise AssertionError("Resposta IST truncada")
        data += part
    return data

def ist_request(source_id, destination_id, host, port, index, version):
    return network_request(source_id, destination_id, host, port, 25, os.urandom(16), struct.pack("!QQ", index, version))

def network_request(source_id, destination_id, host, port, kind, transaction_id, payload):
    header = struct.pack("!BB32s32s16sQII", 1, kind, source_id, destination_id, transaction_id, int(time.time()), len(payload), 0)
    frame = header[:-4] + struct.pack("!I", zlib.crc32(header + payload)) + payload
    with socket.create_connection((host, port), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(frame)
        response_header = exact(sock, 98)
        fields = struct.unpack("!BB32s32s16sQII", response_header)
        response_payload = exact(sock, fields[6])
    assert fields[4] == transaction_id
    assert zlib.crc32(response_header[:-4] + bytes(4) + response_payload) == fields[7]
    return fields[1], response_payload

try:
    if len(entries) < 3:
        raise ValueError("O cenário C5 requer três Super Peers")
    for entry in entries[:2]:
        launch(f"sp-{entry['port']}", [str(bins / "superpeer"), "--config", str(Path(args.config).resolve()), "--port", str(entry["port"])], "NodeID:")
    def common_leader():
        leaders = []
        accepted = []
        for name, path in logs.items():
            if not name.startswith("sp-"):
                continue
            content = path.read_text(errors="replace")
            own = re.findall(r"C4 COORDINATOR NodeID=([0-9a-f]{64})", content)
            other = re.findall(r"C4 COORDINATOR aceito NodeID=([0-9a-f]{64})", content)
            if not own and not other:
                return False
            leaders.append(own[-1] if own and (not other or content.rfind("C4 COORDINATOR NodeID=") > content.rfind("C4 COORDINATOR aceito NodeID=")) else other[-1])
            accepted.append(other[-1] if other else own[-1])
        return len(set(accepted)) == 1 and len(set(leaders)) == 1
    wait_for(common_leader, "eleição e convergência do coordenador", timeout=35)
    peer_port = 0
    import socket
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        peer_port = sock.getsockname()[1]
    launch("peer", [str(bins / "peer"), "serve", str(peer_port), entries[0]["host"], str(entries[0]["port"])], "Peer storage started")
    object_ids = []
    for number in range(2):
        sample = root / f"checkpoint5-{number}.pdf"
        sample.write_bytes(f"Checkpoint 5 2PC integration {number}\n".encode() * 300)
        result = subprocess.run([str(bins / "peer"), "--local-peer-port", str(peer_port), "upload", str(sample), "127.0.0.1", str(peer_port)], cwd=root, env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        match = re.search(r"ObjectID: ([0-9a-f]{64})", result.stdout)
        assert match, result.stdout + result.stderr
        object_ids.append(match.group(1))
    transaction = re.compile(r"C5 PREPARE transaction=[0-9a-f]{32} index=2 version=2")
    wait_for(lambda: all(transaction.search(path.read_text(errors="replace")) for name, path in logs.items() if name.startswith("sp-")), "PREPARE recebido nos Super Peers ativos")
    wait_for(lambda: all(len(re.findall(r"C5 COMMIT transaction=[0-9a-f]{32}", path.read_text(errors="replace"))) >= 2 for name, path in logs.items() if name.startswith("sp-")), "COMMIT aplicado nos Super Peers ativos")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        download_peer_port = sock.getsockname()[1]
    launch("download-peer", [str(bins / "peer"), "serve", str(download_peer_port), entries[0]["host"], str(entries[0]["port"])], "Peer storage started")
    for number, object_id in enumerate(object_ids):
        source_file = root / f"checkpoint5-{number}.pdf"
        for repeat in range(2):
            destination = root / f"download-{number}-{repeat}.pdf"
            result = subprocess.run([str(bins / "peer"), "--local-peer-port", str(download_peer_port), "download", object_id, str(destination), entries[0]["host"], str(entries[0]["port"])], cwd=root, env=env, text=True, capture_output=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            assert destination.read_bytes() == source_file.read_bytes(), f"download {number}/{repeat} divergiu do original"
    peer_log = logs["peer"].read_text(errors="replace")
    assert "LFU CACHE MISS" in peer_log and "LFU CACHE HIT" in peer_log and re.search(r"LFU CACHE HIT .*frequency=2", peer_log), peer_log
    late_entry = entries[2]
    launch(f"sp-{late_entry['port']}", [str(bins / "superpeer"), "--config", str(Path(args.config).resolve()), "--port", str(late_entry["port"])], "NodeID:")
    wait_for(common_leader, "convergência após entrada do Super Peer atrasado", timeout=35)
    late_log = root / ".superpeer_storage" / str(late_entry["port"]) / "c4-operation.log"
    wait_for(lambda: late_log.exists() and late_log.stat().st_size > 0, "recuperação do log no Super Peer atrasado", timeout=35)
    committed_logs = [root / ".superpeer_storage" / str(entry["port"]) / "c4-operation.log" for entry in entries]
    wait_for(lambda: all(path.exists() and path.stat().st_size == committed_logs[0].stat().st_size for path in committed_logs), "catch-up do mesmo número de bytes")
    assert len({path.read_bytes() for path in committed_logs}) == 1, "logs C4 divergiram após IST"
    ids = {}
    for entry in entries:
        content = logs[f"sp-{entry['port']}"].read_text(errors="replace")
        ids[entry["port"]] = bytes.fromhex(re.search(r"NodeID: ([0-9a-f]{64})", content).group(1))
        for object_id in object_ids:
            output = subprocess.run([str(bins / "client"), "--cmd", "lookup", "--object-id", object_id, "--host", entry["host"], "--port", str(entry["port"])], cwd=root, env=env, text=True, capture_output=True, timeout=8)
            assert output.returncode == 0, f"lookup no Super Peer {entry['port']}: {output.stdout}{output.stderr}"
            assert "Lookup concluido" in output.stdout and "Owner:" in output.stdout, output.stdout
    leader_match = re.findall(r"C4 COORDINATOR(?: aceito)? NodeID=([0-9a-f]{64})", logs[f"sp-{entries[0]['port']}"] .read_text(errors="replace"))
    assert leader_match, "nenhum coordenador C4 foi registrado"
    leader_id = bytes.fromhex(leader_match[-1])
    abort_target = next(entry for entry in entries if ids[entry["port"]] != leader_id)
    abort_id = bytes.fromhex("ab" * 16)
    before_abort = (root / ".superpeer_storage" / str(abort_target["port"]) / "c4-operation.log").read_bytes()
    kind, _ = network_request(leader_id, ids[abort_target["port"]], abort_target["host"], abort_target["port"], 12, abort_id, b"")
    assert kind == 1, "ABORT sem PREPARE deveria persistir tombstone e responder ACK"
    abort_payload = b"x"
    abort_header = struct.pack("!4sQQ16sII", b"C4LG", 3, 3, abort_id, len(abort_payload), 0)
    abort_wire = abort_header[:-4] + struct.pack("!I", zlib.crc32(abort_header + abort_payload)) + abort_payload
    kind, _ = network_request(leader_id, ids[abort_target["port"]], abort_target["host"], abort_target["port"], 10, abort_id, abort_wire)
    assert kind == 2, "PREPARE atrasado após ABORT foi aceito"
    assert (root / ".superpeer_storage" / str(abort_target["port"]) / "c4-operation.log").read_bytes() == before_abort, "ABORT publicou entrada no log"
    source_entry = entries[0]
    target_entry = entries[1]
    source_id = ids[source_entry["port"]]
    target_id = ids[target_entry["port"]]
    kind, batch = ist_request(source_id, target_id, target_entry["host"], target_entry["port"], 0, 0)
    assert kind == 26 and struct.unpack("!I", batch[:4])[0] == 2
    kind, batch = ist_request(source_id, target_id, target_entry["host"], target_entry["port"], 1, 1)
    assert kind == 26 and struct.unpack("!I", batch[:4])[0] == 1
    kind, batch = ist_request(source_id, target_id, target_entry["host"], target_entry["port"], 2, 2)
    assert kind == 26 and batch == struct.pack("!I", 0), "resposta para réplica atual deveria ser lote vazio"
    kind, _ = ist_request(source_id, target_id, target_entry["host"], target_entry["port"], 3, 3)
    assert kind == 2, "gap de índice deveria ser rejeitado"
    print(f"2PC/IST TCP: duas ANNOUNCE replicadas e recuperadas pelo Super Peer atrasado; batch, cursor, estado atual e gap OK")
finally:
    for process in processes:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
