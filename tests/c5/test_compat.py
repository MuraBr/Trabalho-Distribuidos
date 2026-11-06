"""Contratos públicos da preparação Bash; integração real é opcional e explícita."""
import os
from pathlib import Path
import socket
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SETUP = ROOT / "tests/c5/compat_env.sh"


class CompatShellTests(unittest.TestCase):
    def shell(self, command, **extra):
        env = dict(os.environ)
        for key in ("BASH_ENV", "C5_COMPAT_ROOT", "C5_COMPAT_SESSION"):
            env.pop(key, None)
        env.update(extra)
        process = subprocess.Popen(["bash", "-c", command], cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            output, errors = process.communicate(timeout=300)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
            raise
        return subprocess.CompletedProcess(command, process.returncode, output, errors)

    def test_source_prepares_only_this_session_and_keeps_normal_bash(self):
        result = self.shell('source tests/c5/compat_env.sh && test -r "$BASH_ENV" && bash -c "printf normal-bash"')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("normal-bash", result.stdout)

    def test_existing_bash_env_is_not_overwritten(self):
        result = self.shell('source tests/c5/compat_env.sh', BASH_ENV="/dev/null")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("BASH_ENV", result.stderr)

    def test_occupied_port_fails_before_build_or_starting_nodes(self):
        with socket.socket() as occupied:
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            occupied.bind(("127.0.0.1", 5101))
            occupied.listen()
            result = self.shell('source tests/c5/compat_env.sh && bash ./tests/c5/run.sh')
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("5101", result.stderr)
            self.assertIn("ocupada", result.stderr)
            occupied.settimeout(.1)
            with self.assertRaises(TimeoutError):
                occupied.accept()

    def test_ignored_client_error_is_still_a_failure(self):
        with tempfile.TemporaryDirectory(prefix="c5-compat-negative-") as temporary:
            session = Path(temporary)
            (session / "error-client.txt").write_text("upload falhou: erro intencional")
            result = self.shell('python3 tests/c5/compat.py verify', C5_COMPAT_SESSION=temporary)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ignorada pelo runner", result.stderr)
            self.assertIn("erro intencional", result.stderr)

    def test_unexpected_node_exit_is_not_ignored(self):
        with tempfile.TemporaryDirectory(prefix="c5-compat-node-failure-") as temporary:
            session = Path(temporary)
            (session / "bin").mkdir()
            (session / "bin/superpeer").symlink_to("/bin/false")
            result = self.shell('python3 tests/c5/compat.py node --config unused --port 5101', C5_COMPAT_SESSION=temporary)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(list(session.glob("error-*.txt")), "saída inesperada do nó deve ficar registrada")

    @unittest.skipUnless(os.environ.get("C5_COMPAT_E2E") == "1", "ative C5_COMPAT_E2E=1 para executar processos reais")
    def test_original_runner_and_real_evidence_preserve_repository(self):
        paths = [ROOT / item for item in ("tests/c5/run.sh", "tests/config/c5.conf", "tests/common.sh", "tests/env.sh", "tests/data/SHA256SUMS")]
        before = {path: path.read_bytes() for path in paths}
        result = self.shell('source tests/c5/compat_env.sh && bash ./tests/c5/run.sh')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: 5", result.stdout)
        self.assertIn("FAIL: 0", result.stdout)
        self.assertIn("verificações adicionais aprovados", result.stdout)
        for path, expected in before.items():
            self.assertEqual(path.read_bytes(), expected, str(path))
        for port in (5101, 5102, 5103):
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", port))


if __name__ == "__main__":
    unittest.main()
