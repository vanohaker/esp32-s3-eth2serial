"""Prepare a persistent local TLS identity to embed in the firmware, never in Git."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def openssl(*args):
    return subprocess.check_output(["openssl", *map(str, args)], stderr=subprocess.PIPE, timeout=30)


def validate(cert, key):
    openssl("verify", "-check_ss_sig", "-CAfile", cert, "-purpose", "sslserver",
            "-verify_hostname", "serial-dongle", cert)
    public_cert = openssl("x509", "-in", cert, "-pubkey", "-noout")
    public_key = openssl("pkey", "-in", key, "-passin", "pass:", "-pubout")
    if public_cert != public_key:
        raise ValueError("Certificate and private key do not match; refusing to replace them")


def prepare(directory):
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory.chmod(0o700)
    cert, key = directory / "https_cert.pem", directory / "https_key.pem"
    if cert.exists() or key.exists():
        if not cert.is_file() or not key.is_file():
            raise ValueError("Incomplete TLS identity; restore both files from backup")
        cert.chmod(0o600)
        key.chmod(0o600)
        validate(cert, key)
        return
    # Temporary private files stay within the ignored directory, including if
    # the build is interrupted. Never silently rotate an existing identity.
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        pending_cert, pending_key = Path(temporary) / cert.name, Path(temporary) / key.name
        openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-sha256", "-noenc", "-batch", "-days", "7300", "-subj", "/CN=serial-dongle",
                "-addext", "basicConstraints=critical,CA:FALSE",
                "-addext", "keyUsage=critical,digitalSignature",
                "-addext", "extendedKeyUsage=serverAuth",
                "-addext", "subjectAltName=DNS:serial-dongle",
                "-keyout", pending_key, "-out", pending_cert)
        pending_cert.chmod(0o600)
        pending_key.chmod(0o600)
        validate(pending_cert, pending_key)
        pending_key.replace(key)
        pending_cert.replace(cert)
    print("Prepared fixed HTTPS identity in ignored .tls directory")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parents[1] / ".tls")
    args = parser.parse_args()
    os.umask(0o077)
    try:
        prepare(args.directory)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        # Do not echo command output or key material on failure.
        parser.exit(1, f"Cannot prepare embedded TLS identity: {type(error).__name__}. "
                       "Check OpenSSL and the local certificate/key pair; files were not replaced.\n")
