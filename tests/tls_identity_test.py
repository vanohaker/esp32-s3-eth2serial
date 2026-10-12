"""Fixed TLS identity: valid server profile, stable rebuilds, no silent replacement."""
import hashlib
import importlib.util
from pathlib import Path
import socket
import ssl
import tempfile
import threading
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/prepare_tls.py"
spec = importlib.util.spec_from_file_location("prepare_tls", SCRIPT)
prepare_tls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_tls)


class IdentityTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name) / "tls"
        prepare_tls.prepare(self.directory)
        self.cert = self.directory / "https_cert.pem"
        self.key = self.directory / "https_key.pem"

    def snapshot(self):
        # Hashes prevent assertions from ever printing private key material.
        return [(hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                for p in [self.cert, self.key]]

    def test_repeated_build_preserves_identity_and_permissions(self):
        before = self.snapshot()
        prepare_tls.prepare(self.directory)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.directory.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.key.stat().st_mode & 0o777, 0o600)

    def test_incomplete_identity_is_not_replaced(self):
        self.cert.unlink()
        digest = hashlib.sha256(self.key.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            prepare_tls.prepare(self.directory)
        self.assertFalse(self.cert.exists())
        self.assertEqual(hashlib.sha256(self.key.read_bytes()).hexdigest(), digest)

    def test_mismatched_identity_is_not_replaced(self):
        other = Path(self.temporary.name) / "other"
        prepare_tls.prepare(other)
        self.key.write_bytes((other / self.key.name).read_bytes())
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, "do not match"):
            prepare_tls.prepare(self.directory)
        self.assertEqual(self.snapshot(), before)

    def test_real_tls_handshake_with_exact_certificate_trust(self):
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(self.cert, self.key)
        client_context = ssl.create_default_context(cafile=str(self.cert))
        client_context.hostname_checks_common_name = False
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)

            def serve():
                try:
                    connection, _ = listener.accept()
                    with server_context.wrap_socket(connection, server_side=True) as tls:
                        tls.settimeout(5)
                        tls.sendall(b"ok")
                except Exception as error:
                    errors.append(type(error).__name__)

            thread = threading.Thread(target=serve)
            thread.start()
            try:
                with socket.create_connection(listener.getsockname(), timeout=5) as connection:
                    with client_context.wrap_socket(connection, server_hostname="serial-dongle") as tls:
                        self.assertIn(("DNS", "serial-dongle"), tls.getpeercert()["subjectAltName"])
                        self.assertEqual(tls.recv(2), b"ok")
            finally:
                thread.join(timeout=6)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors, errors)


if __name__ == "__main__":
    unittest.main()
