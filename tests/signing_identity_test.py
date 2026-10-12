"""OTA private keys must never be regenerated or silently replaced during builds."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("signing", Path(__file__).resolve().parents[1] / "tools/prepare_signing.py")
signing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signing)


class SigningIdentityTests(unittest.TestCase):
    def test_stable_identity_and_exclusive_initialization(self):
        with tempfile.TemporaryDirectory() as root:
            key, public = Path(root) / "private/key.pem", Path(root) / "public.pem"
            signing.prepare(key, public, True)
            original = key.read_bytes(), public.read_bytes(), key.stat().st_mtime_ns
            signing.prepare(key, public)
            with self.assertRaises(ValueError):
                signing.prepare(key, public, True)
            self.assertEqual(original, (key.read_bytes(), public.read_bytes(), key.stat().st_mtime_ns))

    def test_missing_key_requires_restore(self):
        with tempfile.TemporaryDirectory() as root:
            key, public = Path(root) / "key.pem", Path(root) / "public.pem"
            signing.prepare(key, public, True)
            expected = public.read_bytes()
            key.unlink()
            for initialize in (False, True):
                with self.assertRaises(ValueError):
                    signing.prepare(key, public, initialize)
            self.assertFalse(key.exists())
            self.assertEqual(public.read_bytes(), expected)

    def test_foreign_private_key_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            a, b = Path(root) / "a.pem", Path(root) / "b.pem"
            ap, bp = Path(root) / "a.pub", Path(root) / "b.pub"
            signing.prepare(a, ap, True)
            signing.prepare(b, bp, True)
            with self.assertRaises(ValueError):
                signing.prepare(a, bp)

    def test_world_readable_key_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            key, public = Path(root) / "key.pem", Path(root) / "public.pem"
            signing.prepare(key, public, True)
            key.chmod(0o644)
            with self.assertRaises(ValueError):
                signing.prepare(key, public)


if __name__ == "__main__":
    unittest.main()
