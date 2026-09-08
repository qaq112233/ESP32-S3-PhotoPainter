"""Generate disposable real certificates for the pinned mbedTLS SAN parser."""
import subprocess
import sys
import tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="photopull-cert-") as directory:
    root = Path(directory)
    cases = [
        ("ip", "127.0.0.1", "IP:127.0.0.1"),
        ("wrong", "127.0.0.2", "IP:127.0.0.2"),
        ("cn", "127.0.0.1", None),
        ("dns", "127.0.0.1", "DNS:127.0.0.1"),
        ("v6", "localhost", "IP:2001:db8::1"),
    ]
    for label, cn, san in cases:
        command = [sys.argv[2], "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                   "-keyout", str(root / (label + ".key")), "-out", str(root / (label + ".pem")),
                   "-days", "1", "-subj", "/CN=" + cn]
        if san:
            command += ["-addext", "subjectAltName=" + san]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([sys.argv[1], str(root)], check=True)
