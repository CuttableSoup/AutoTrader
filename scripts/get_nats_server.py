#!/usr/bin/env python
"""Download the nats-server binary for this platform into tools/nats/.

usage: python scripts/get_nats_server.py [--version v2.14.7]
"""
from __future__ import annotations

import argparse
import io
import json
import platform
import stat
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None, help="e.g. v2.14.7 (default: latest release)")
    a = ap.parse_args()
    version = a.version
    if not version:
        with urllib.request.urlopen("https://api.github.com/repos/nats-io/nats-server/releases/latest", timeout=30) as r:
            version = json.load(r)["tag_name"]
    sysname = platform.system().lower()
    arch = {"x86_64": "amd64", "amd64": "amd64", "arm64": "arm64", "aarch64": "arm64"}[platform.machine().lower()]
    if sysname == "windows":
        name, ext, binname = f"nats-server-{version}-windows-{arch}", "zip", "nats-server.exe"
    elif sysname == "darwin":
        name, ext, binname = f"nats-server-{version}-darwin-{arch}", "tar.gz", "nats-server"
    else:
        name, ext, binname = f"nats-server-{version}-linux-{arch}", "tar.gz", "nats-server"
    url = f"https://github.com/nats-io/nats-server/releases/download/{version}/{name}.{ext}"
    out_dir = ROOT / "tools" / "nats"
    out_dir.mkdir(parents=True, exist_ok=True)
    print("downloading", url)
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    target = out_dir / binname
    if ext == "zip":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            member = next(n for n in z.namelist() if n.endswith(binname))
            target.write_bytes(z.read(member))
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            member = next(m for m in t.getmembers() if m.name.endswith(binname))
            target.write_bytes(t.extractfile(member).read())
        target.chmod(target.stat().st_mode | stat.S_IXUSR)
    print("wrote", target)
    sys.exit(0)


if __name__ == "__main__":
    main()
