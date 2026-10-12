#!/usr/bin/env python3
"""Validate the project's OTA signing key. Initialization is explicit, never automatic."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def openssl(*args):
    return subprocess.run(["openssl", *map(str, args)], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def prepare(key: Path, public: Path, initialize=False):
    if initialize:
        if key.exists() or public.exists():
            raise ValueError("Refusing to replace an existing OTA signing identity")
        key.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        public.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=key.parent) as directory:
            temporary = Path(directory) / "key.pem"
            openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:3072", "-out", temporary)
            temporary.chmod(0o600)
            pub = openssl("pkey", "-in", temporary, "-pubout")
            # Exclusive creation: another process must not replace our trust anchor.
            with key.open("xb") as dest:
                dest.write(temporary.read_bytes())
            key.chmod(0o600)
            with public.open("xb") as dest:
                dest.write(pub)
    if not key.is_file() or not public.is_file():
        raise ValueError("OTA key missing: restore .signing/ota-key.pem from your private backup")
    if key.stat().st_mode & 0o077:
        raise ValueError("OTA private key must have permissions 0600")
    openssl("rsa", "-in", key, "-check", "-noout")
    actual = openssl("pkey", "-in", key, "-pubout", "-outform", "DER")
    expected = openssl("pkey", "-pubin", "-in", public, "-outform", "DER")
    info = openssl("pkey", "-pubin", "-in", public, "-text", "-noout")
    if actual != expected or b"(3072 bit)" not in info:
        raise ValueError("OTA signing key must be RSA-3072 and match firmware-signing-public.pem")


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--initialize", action="store_true")
    parser.add_argument("--key", type=Path, default=ROOT / ".signing/ota-key.pem")
    parser.add_argument("--public", type=Path, default=ROOT / "firmware-signing-public.pem")
    args = parser.parse_args()
    try:
        prepare(args.key, args.public, args.initialize)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"OTA signing identity unavailable: {error if isinstance(error, ValueError) else 'OpenSSL or filesystem error'}\n")


if __name__ == "__main__":
    main()
