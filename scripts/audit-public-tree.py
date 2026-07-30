#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_SUFFIXES = {'.pup','.self','.sprx','.prx','.pkg','.fself','.sfo','.rif','.keystone','.dec','.bin','.elf','.img','.pem','.key','.p12','.pfx','.der'}
MAX_SIZE = 5 * 1024 * 1024
ALLOWED_BINARY = {
    Path('assets/dropbear-ssh-ps4-logo.png'): '67d5f7223d031b1e6b5ad7b224eb8f8c57beba2898036be4c356de0acf8b4eab',
    Path('packaging/installer-v107/icon0.png'): '67d5f7223d031b1e6b5ad7b224eb8f8c57beba2898036be4c356de0acf8b4eab',
    Path('dropbear/libtomcrypt/doc/libtomsm.png'): 'd41c47d187d12908d464dc4ad565b42685143f2308c15d5fb3129968392f3f2f',
}
PATTERNS = {
    'private-key': re.compile(rb'(?m)^-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----\r?$'),
    'github-token': re.compile(rb'(?:github_pat_|gh[pousr]_)[A-Za-z0-9_]{20,}'),
    'aws-key': re.compile(rb'AKIA[0-9A-Z]{16}'),
    'local-path': re.compile(rb'/home/hermes/'),
    'private-ip': re.compile(rb'(?<![0-9])(?:10(?:\.[0-9]{1,3}){3}|192\.168(?:\.[0-9]{1,3}){2}|172\.(?:1[6-9]|2[0-9]|3[01])(?:\.[0-9]{1,3}){2})(?![0-9])'),
}

def tracked() -> list[Path]:
    out=subprocess.check_output(['git','-C',str(ROOT),'ls-files','-z'])
    return [Path(x.decode()) for x in out.split(b'\0') if x]

def main() -> int:
    failures=[]; files=tracked()
    for rel in files:
        path=ROOT/rel
        if path.is_symlink(): failures.append(f'symlink: {rel}'); continue
        if not path.is_file(): continue
        data=path.read_bytes()
        if path.stat().st_size > MAX_SIZE: failures.append(f'oversized: {rel}')
        if path.suffix.lower() in FORBIDDEN_SUFFIXES: failures.append(f'forbidden suffix: {rel}')
        if rel in ALLOWED_BINARY:
            digest=hashlib.sha256(data).hexdigest()
            if digest != ALLOWED_BINARY[rel]: failures.append(f'logo hash mismatch: {rel}')
            continue
        if b'\0' in data[:8192]: failures.append(f'unexpected binary: {rel}')
        if rel == Path('scripts/audit-public-tree.py'): continue
        for label,rx in PATTERNS.items():
            if rx.search(data): failures.append(f'{label}: {rel}')
    missing=set(ALLOWED_BINARY)-set(files)
    failures.extend(f'missing required artwork: {p}' for p in sorted(missing))
    if failures:
        print('FAIL:'); print('\n'.join(f'  {x}' for x in failures)); return 1
    print(f'PASS: audited {len(files)} tracked files; artwork hashes verified'); return 0
if __name__ == '__main__': sys.exit(main())
