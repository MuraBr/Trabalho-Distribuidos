#!/usr/bin/env python3
"""Valida eleição Bully, ANNOUNCE replicado e recuperação do coordenador por TCP."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--bin-dir", default=str(Path(__file__).resolve().parents[2] / "bin"))
args = parser.parse_args()
bins = Path(args.bin_dir).resolve()
root = Path(tempfile.mkdtemp(prefix="pd-c4-integration-"))
processes = []
ports = []
passed = 0


def check(condition, description):
    global passed
    if not condition:
        raise AssertionError(description)
    passed += 1
    print(f"PASS {description}", flush=True)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait(predicate, timeout=20, description="condição TCP"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(f"Prazo esgotado: {description}")


def log_text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def launch_sp(index):
    directory = root / f"sp-{index}"
    directory.mkdir()
    logfile = directory / "process.log"
    command = [str(bins / "superpeer"), "--port", str(ports[index]), "--data-dir", str(directory)]
    if index > 0:
        command += ["--chord-host", "127.0.0.1", "--chord-port", str(ports[0])]
    output = logfile.open("w")
    process = subprocess.Popen(command, cwd=root, stdout=output, stderr=subprocess.STDOUT, env=env)
    processes.append((process, output, logfile, directory))
    wait(lambda: process.poll() is not None or "NodeID: " in log_text(logfile), description=f"SP {index} iniciar")
    check(process.poll() is None, f"Super Peer {index} iniciou")
    match = re.search(r"NodeID: ([0-9a-f]{64})", log_text(logfile))
    if match is None:
        raise AssertionError(log_text(logfile))
    return bytes.fromhex(match.group(1))


def latest_coordinator(index):
    logfile = processes[index][2]
    matches = re.findall(r"C4 COORDINATOR (?:NodeID=|aceito NodeID=)([0-9a-f]{64})", log_text(logfile))
    return bytes.fromhex(matches[-1]) if matches else None


def all_report_coordinator(node_id, indices):
    return all(latest_coordinator(index) == node_id for index in indices)


def start_peer(superpeer_port):
    directory = root / "peer"
    directory.mkdir()
    logfile = directory / "process.log"
    peer_port = free_port()
    output = logfile.open("w")
    process = subprocess.Popen([str(bins / "peer"), "serve", str(peer_port), "127.0.0.1", str(superpeer_port), "--data-dir", str(directory)], cwd=root, stdout=output, stderr=subprocess.STDOUT, env=env)
    processes.append((process, output, logfile, directory))
    wait(lambda: process.poll() is not None or "Peer storage started" in log_text(logfile), description="Peer iniciar")
    check(process.poll() is None, "Peer de armazenamento iniciou e fez JOIN")
    return process, peer_port


def upload(peer_port, path):
    result = subprocess.run([str(bins / "peer"), "--local-peer-port", str(peer_port), "upload", str(path), "127.0.0.1", str(peer_port)], cwd=root, env=env, text=True, capture_output=True, timeout=30)
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    check(f"ObjectID: {digest}" in result.stdout, f"upload confirmou ObjectID {digest[:12]}")


def log_bytes(index):
    return (root / f"sp-{index}" / "c4-operation.log").read_bytes()


env = dict(os.environ, C3_HEARTBEAT_MS="300", C3_SUSPECT_MS="1200", C3_FAILED_MS="2500", C3_REMOVED_MS="5000", C3_RPC_MS="250", PEER_IO_TIMEOUT="2", PEER_CONNECT_TIMEOUT="1")
ports = [free_port() for _ in range(3)]
while len(set(ports)) != len(ports):
    ports = [free_port() for _ in range(3)]

try:
    ids = [launch_sp(0)]
    ids.append(launch_sp(1))
    ids.append(launch_sp(2))
    leader = max(ids)
    wait(lambda: all_report_coordinator(leader, [0, 1, 2]), timeout=25, description="Bully escolher o maior NodeID em todos os membros")
    check(all_report_coordinator(leader, [0, 1, 2]), "Bully elegeu o maior NodeID conhecido")

    peer, peer_port = start_peer(ports[(ids.index(leader) + 1) % 3])
    first = root / "first.pdf"
    first.write_bytes(b"PDF checkpoint 4, operacao replicada\n" * 200)
    upload(peer_port, first)
    wait(lambda: all((root / f"sp-{index}" / "c4-operation.log").exists() for index in range(3)), description="logs de réplica criados")
    wait(lambda: len({log_bytes(index) for index in range(3)}) == 1, description="ANNOUNCE replicado em todos os logs")
    check(all(len(log_bytes(index)) > 44 for index in range(3)), "ANNOUNCE persistido com índice, versão, TransactionID e checksum nas réplicas")

    leader_index = ids.index(leader)
    first_log_size = len(log_bytes(0))
    leader_process = processes[leader_index][0]
    leader_process.send_signal(signal.SIGKILL)
    leader_process.wait(timeout=5)
    remaining = [node_id for index, node_id in enumerate(ids) if index != leader_index]
    survivor_indices = [index for index in range(3) if index != leader_index]
    replacement = max(remaining)
    failed_pattern = re.compile(rf"Membership {leader.hex()} SUSPECT -> FAILED")
    wait(lambda: any(failed_pattern.search(log_text(item[2])) for index, item in enumerate(processes[:3]) if index != leader_index), timeout=15, description="C3 declarar o coordenador anterior FAILED")
    failed_at = time.monotonic()
    wait(lambda: all_report_coordinator(replacement, survivor_indices), timeout=5, description="nova eleição Bully após FAILED")
    check(time.monotonic() - failed_at < 5.0, "recuperação do coordenador concluída em menos de cinco segundos após FAILED")
    check(len({log_bytes(index) for index in survivor_indices}) == 1, "réplicas sobreviventes preservaram o mesmo log após a queda")

    second = root / "second.pdf"
    second.write_bytes(b"PDF nova operacao apos eleicao\n" * 250)
    upload(peer_port, second)
    wait(lambda: len({log_bytes(index) for index in survivor_indices}) == 1 and len(log_bytes(survivor_indices[0])) > first_log_size, description="nova operação replicada após eleição")
    check(len(log_bytes(survivor_indices[0])) > first_log_size, "nova operação aceita pelo coordenador reeleito e persistida nas réplicas")
    print(f"RESULTADO C4: {passed} verificações aprovadas; evidências: {root}", flush=True)
except Exception:
    print(f"Evidências C4 preservadas: {root}", flush=True)
    for _, _, logfile, _ in processes:
        print(f"--- {logfile} (últimas 40 linhas) ---\n" + "\n".join(log_text(logfile).splitlines()[-40:]), flush=True)
    raise
finally:
    for process, output, _, _ in processes:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        output.close()
