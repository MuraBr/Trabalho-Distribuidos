#!/usr/bin/env python3
"""Adaptadores explícitos do runner C5; sem alterar o programa C ou simular sucesso."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
TIMEOUT = 40


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value), encoding="utf-8")
    temporary.replace(path)


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def wait_for(predicate, description, timeout=TIMEOUT):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise RuntimeError(f"Prazo excedido: {description}")


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def request(host, port, source, destination, kind, payload):
    """Sonda TCP com framing real, CRC e correlação; não gera respostas artificiais."""
    transaction = os.urandom(16)
    header = struct.pack("!BB32s32s16sQII", 1, kind, bytes.fromhex(source), bytes.fromhex(destination), transaction, int(time.time()), len(payload), 0)
    frame = header[:-4] + struct.pack("!I", zlib.crc32(header + payload)) + payload

    def exact(connection, size):
        result = bytearray()
        while len(result) < size:
            part = connection.recv(size - len(result))
            if not part:
                raise RuntimeError("Resposta TCP truncada")
            result.extend(part)
        return bytes(result)

    with socket.create_connection((host, port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(frame)
        raw_header = exact(connection, 98)
        fields = struct.unpack("!BB32s32s16sQII", raw_header)
        if fields[0] != 1 or fields[6] > 5 * 1024 * 1024 or fields[4] != transaction or fields[2].hex() != destination or fields[3].hex() != source:
            raise RuntimeError("Header de resposta incompatível")
        response = exact(connection, fields[6])
        if zlib.crc32(raw_header[:-4] + bytes(4) + response) != fields[7]:
            raise RuntimeError("CRC de resposta inválido")
        return fields[1], response


def preflight():
    # Não conecta nem encerra serviços alheios: apenas verifica bind exclusivo.
    listeners = []
    try:
        for port in (5101, 5102, 5103):
            listener = socket.socket()
            listeners.append(listener)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                listener.bind(("127.0.0.1", port))
                listener.listen()
            except OSError as error:
                raise RuntimeError(f"Porta {port} ocupada; encerre seu teste anterior ou use outra sessão de rede.") from error
    finally:
        for listener in listeners:
            listener.close()


def node_id(directory, port):
    return hashlib.sha256(socket.inet_aton("127.0.0.1") + struct.pack("!H", port) + (directory / "node.uuid").read_bytes()).hexdigest()


def common_leader(session, ports=(5101, 5102, 5103)):
    leaders = []
    own = []
    for port in ports:
        content = text(session / f"sp-{port}.log")
        ids = re.findall(r"NodeID: ([0-9a-f]{64})", content)
        announcements = re.findall(r"C4 COORDINATOR(?: aceito)? NodeID=([0-9a-f]{64})", content)
        if not ids or not announcements:
            return None
        own.append(ids[0])
        leaders.append(announcements[-1])
    return leaders[0] if len(set(leaders)) == 1 and leaders[0] == max(own) else None


def run_command(session, arguments, timeout=TIMEOUT):
    result = subprocess.run(arguments, cwd=session, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Comando falhou ({result.returncode}): {shlex.join(map(str, arguments))}\n{result.stdout}{result.stderr}")
    print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)
    return result.stdout


def start_background(session, name, arguments, marker):
    path = session / f"{name}.log"
    with path.open("ab") as output:
        process = subprocess.Popen(arguments, cwd=session, stdout=output, stderr=subprocess.STDOUT)
    save(session / f"{name}.pid.json", process.pid)

    def ready():
        if process.poll() is not None:
            raise RuntimeError(f"{name} encerrou prematuramente: {text(path)}")
        return marker in text(path)

    wait_for(ready, f"inicialização de {name}")
    return process


def node_adapter(session, arguments):
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--port", type=int, required=True)
    options = parser.parse_args(arguments)
    if options.port not in (5101, 5102, 5103):
        raise RuntimeError("Porta fora do contrato C5")
    path = session / f"sp-{options.port}.log"
    command = [str(session / "bin/superpeer"), "--config", options.config, "--port", str(options.port), "--data-dir", str(session / f"sp-{options.port}")]
    process = subprocess.Popen(command, cwd=session, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    stopping = False

    def terminate(signum, frame):
        nonlocal stopping
        stopping = True
        if process.poll() is None:
            process.terminate()

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    try:
        with path.open("ab", buffering=0) as output:
            while True:
                line = process.stdout.readline()
                if not line:
                    break
                output.write(line)
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
        status = process.wait(timeout=10)
        if not stopping:
            (session / f"error-node-{options.port}.txt").write_text(f"Super Peer {options.port} encerrou inesperadamente (código {status}).")
        return status
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def ensure_peer(session):
    state = session / "peer.json"
    if state.exists():
        return load(state)["port"]
    wait_for(lambda: common_leader(session), "convergência dos três Super Peers")
    port = free_port()
    start_background(session, "peer", [str(session / "bin/peer"), "serve", str(port), "127.0.0.1", "5101", "--data-dir", str(session / "peer")], "Peer storage started")
    save(state, {"port": port})
    return port


def download(session, port, selector, host, remote_port, destination):
    return run_command(session, [str(session / "bin/peer"), "--local-peer-port", str(port), "download", selector, str(destination), host, str(remote_port)])


def recovered_logs(session):
    expected = (session / "sp-5101/c4-operation.log").read_bytes()
    actual = session / "late/c4-operation.log"
    if not expected or not actual.exists() or actual.read_bytes() != expected:
        return False
    if not all((session / f"sp-{port}/c4-operation.log").read_bytes() == expected for port in (5101, 5102, 5103)):
        return False
    return True


def late_join(session, upload, host, port):
    # O JOIN legado continua acontecendo; o cenário adicional é identificado no log.
    run_command(session, [str(session / "bin/peer"), "--cmd", "join", "--host", host, "--port", str(port)])
    leader = wait_for(lambda: common_leader(session), "liderança antes do nó atrasado")
    late_port = free_port()
    directory = session / "late"
    directory.mkdir()
    # Mantém o foco no catch-up: a identidade do nó de teste fica abaixo dos ativos.
    minimum = min(node_id(session / f"sp-{p}", p) for p in (5101, 5102, 5103))
    for _ in range(10000):
        uuid = os.urandom(16)
        identity = hashlib.sha256(socket.inet_aton("127.0.0.1") + struct.pack("!H", late_port) + uuid).hexdigest()
        if identity < minimum:
            break
    else:
        raise RuntimeError("Não foi possível preparar a identidade do nó de teste")
    (directory / "node.uuid").write_bytes(uuid)
    start_background(session, "late", [str(session / "bin/superpeer"), "--port", str(late_port), "--chord-host", host, "--chord-port", str(port), "--data-dir", str(directory)], "NodeID:")
    wait_for(lambda: recovered_logs(session), "recuperação do histórico do Super Peer atrasado")
    # Confirma tráfego IST originado pelo nó atrasado, não apenas igualdade de arquivos.
    wait_for(lambda: any(re.search(rf"tipo=25, origem={identity}", text(session / f"sp-{p}.log")) for p in (5101, 5102, 5103)), "requisição IST real do nó atrasado")
    # O catálogo filtra owners por local_registration: gossip não equivale a JOIN local.
    # Registra explicitamente o endpoint real já ativo; não reenvia ANNOUNCE nem chunks.
    peer_port = ensure_peer(session)
    peer_id = node_id(session / "peer", peer_port)
    uuid = (session / "peer/node.uuid").read_bytes()
    incarnation = (session / "peer/node.incarnation").read_bytes()
    if len(incarnation) != 8:
        raise RuntimeError("Incarnação inválida do Peer ativo")
    payload = b"127.0.0.1".ljust(46, b"\0") + struct.pack("!H", peer_port) + uuid + b"\x01" + incarnation
    kind, _ = request(host, late_port, peer_id, identity, 0, payload)
    if kind != 1:
        raise RuntimeError("Nó atrasado não aceitou o cadastro adicional do endpoint ativo")
    print("[COMPAT] Endpoint do Peer ativo cadastrado também no nó atrasado; nenhum novo ANNOUNCE foi enviado.", flush=True)
    target = session / "downloads/late.pdf"
    download(session, peer_port, upload["object_id"], host, late_port, target)
    if hashlib.sha256(target.read_bytes()).hexdigest() != upload["object_id"]:
        raise RuntimeError("Catálogo recuperado produziu conteúdo divergente")
    save(session / "ist.json", {"leader": leader, "node_id": identity, "port": late_port, "verified": True})
    print("[COMPAT] IST verificado: requisição TCP, histórico idêntico e download pelo catálogo do nó atrasado.", flush=True)


def client_adapter(session, arguments):
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmd", choices=("upload", "lookup", "join"), required=True)
    parser.add_argument("--file")
    parser.add_argument("--name")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5101)
    options = parser.parse_args(arguments)
    port = ensure_peer(session)
    if options.cmd == "upload":
        if not options.file:
            raise RuntimeError("Upload exige --file")
        path = Path(options.file).resolve()
        expected = hashlib.sha256(path.read_bytes()).hexdigest()
        print("[COMPAT] Upload executado pelo Peer ativo, com sua identidade.", flush=True)
        report = run_command(session, [str(session / "bin/peer"), "--local-peer-port", str(port), "upload", str(path), "127.0.0.1", str(port)])
        if f"ObjectID: {expected}" not in report or "Upload completed" not in report:
            raise RuntimeError("Upload não confirmou o conteúdo esperado")
        wait_for(lambda: all("C5 PREPARE transaction=" in text(session / f"sp-{p}.log") and "C5 COMMIT transaction=" in text(session / f"sp-{p}.log") for p in (5101, 5102, 5103)), "fases reais de transação nos participantes")
        save(session / "upload.json", {"object_id": expected, "file": str(path), "name": path.name})
        return
    upload = load(session / "upload.json")
    if options.cmd == "lookup":
        if options.name != upload["name"]:
            raise RuntimeError("Nome solicitado não corresponde ao upload deste cenário")
        count_path = session / "downloads.json"
        count = load(count_path) if count_path.exists() else 0
        print("[COMPAT] Consulta por nome seguida de download real para exercitar o cache.", flush=True)
        destination = session / f"downloads/lookup-{count}.pdf"
        download(session, port, options.name, options.host, options.port, destination)
        if destination.read_bytes() != Path(upload["file"]).read_bytes():
            raise RuntimeError("Download difere do original")
        if count == 0:
            wait_for(lambda: "LFU CACHE MISS" in text(session / "peer.log"), "MISS real")
        else:
            wait_for(lambda: re.search(r"LFU CACHE HIT .*frequency=(?:[2-9]|[1-9][0-9]+)\b", text(session / "peer.log")), "HIT real após releitura")
        for line in text(session / "peer.log").splitlines():
            if line.startswith("LFU CACHE "):
                print(line, flush=True)
        save(count_path, count + 1)
    else:
        late_join(session, upload, options.host, options.port)


def verify_results(session):
    errors = sorted(session.glob("error-*.txt"))
    if errors:
        raise RuntimeError("Falha de adaptador ignorada pelo runner:\n" + "\n".join(text(path) for path in errors))
    if not (session / "upload.json").exists() or not (session / "downloads.json").exists() or load(session / "downloads.json") < 2 or not (session / "ist.json").exists() or not load(session / "ist.json").get("verified"):
        raise RuntimeError("Runner não concluiu upload, duas leituras e recuperação real")


def prepare(session):
    for relative in ("tests/c5/run.sh", "tests/common.sh", "tests/env.sh", "tests/config/c5.conf", "tests/generate_fixtures.sh"):
        source = ROOT / relative
        target = session / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        if target.read_bytes() != source.read_bytes():
            raise RuntimeError(f"Cópia divergente: {relative}")
    (session / "tests/generate_fixtures.sh").chmod(0o700)
    (session / "downloads").mkdir()
    (session / "adapters").mkdir()
    for mode in ("node", "client"):
        path = session / "adapters" / mode
        path.write_text(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} {shlex.quote(str(Path(__file__).resolve()))} {mode} \"$@\"\n")
        path.chmod(0o700)


def execute_runner():
    lock_path = Path(tempfile.gettempdir()) / f"pd-c5-compat-{os.getuid()}.lock"
    with lock_path.open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Outra execução da compatibilidade C5 está ativa.") from error
        preflight()
        session = Path(tempfile.mkdtemp(prefix="pd-c5-compat-"))
        print(f"[COMPAT] Dados, binários e evidências: {session}", flush=True)
        prepare(session)
        env = dict(os.environ)
        env.pop("BASH_ENV", None)
        env.update(PROJECT_ROOT=str(session), C5_COMPAT_SESSION=str(session), NODE_BIN=str(session / "adapters/node"), CLIENT_BIN=str(session / "adapters/client"), LOG_DIR=str(session / "logs"), PEER_CACHE_BYTES="16777216", C3_HEARTBEAT_MS="300", C3_SUSPECT_MS="1200", C3_FAILED_MS="2500", C3_REMOVED_MS="5000", C3_RPC_MS="500")
        subprocess.run(["make", "-C", str(ROOT), f"BIN_DIR={session / 'bin'}", "CFLAGS=-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror", "all"], env=env, check=True, timeout=90)
        process = subprocess.Popen(["bash", str(session / "tests/c5/run.sh")], cwd=session, env=env, start_new_session=True)

        def interrupt(signum, frame):
            raise KeyboardInterrupt

        previous = {sig: signal.signal(sig, interrupt) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            code = process.wait(timeout=180)
            verify_results(session)
            if code:
                raise RuntimeError(f"Runner original reprovado: código {code}; consulte {session / 'logs/c5.log'}")
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
            # Grupo exclusivo da execução: nunca usa pkill nem PIDs de serviços alheios.
            try:
                os.killpg(process.pid, signal.SIGTERM)
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    time.sleep(.1)
                    os.killpg(process.pid, 0)
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        # print("[COMPAT] Runner original e verificações adicionais aprovados.", flush=True)
        return 0


def main():
    try:
        mode = sys.argv[1]
        if mode == "run":
            return execute_runner()
        session = Path(os.environ["C5_COMPAT_SESSION"])
        if mode == "verify":
            verify_results(session)
            return 0
        if mode == "node":
            try:
                return node_adapter(session, sys.argv[2:])
            except (Exception, SystemExit) as error:
                (session / f"error-node-{os.getpid()}.txt").write_text(str(error))
                raise RuntimeError(str(error)) from error
        if mode != "client":
            raise RuntimeError("Modo desconhecido")
        try:
            client_adapter(session, sys.argv[2:])
        except (Exception, SystemExit) as error:
            (session / f"error-{os.getpid()}.txt").write_text(str(error))
            raise RuntimeError(str(error)) from error
        return 0
    except (Exception, KeyboardInterrupt) as error:
        print(f"[COMPAT ERRO] {error or 'Execução interrompida'}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
