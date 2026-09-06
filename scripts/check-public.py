#!/usr/bin/env python3
"""Conservative release tripwire, not a proof that arbitrary secrets are absent.

Scans tracked working files (source allowlist before git initialization), all
local git objects with --history, and release payloads with --artifacts DIR.
Findings contain labels and rule names only, never matching content.
"""
import argparse
import io
import ipaddress
import pathlib
import re
import subprocess
import sys
import tarfile
import zipfile

RULES = {
    'credential': re.compile(r'(?:sk-[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[A-Z0-9]{16}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)'),
    'private-home-path': re.compile(r'(?:/(?:home|Users)/[A-Za-z0-9_.-]+|[A-Za-z]:\\Users\\[^\\\s]+|/roo[t]/)'),
    'private-collaboration': re.compile(r'collaboration[/\\](?:tasks|handoff|participants|GUIDE)|(?:^|[\s/])(?:grok|codex)\.md\b'),
    'private-domain': re.compile(r'\b[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*\.(?:internal|lan|local)\b'),
}
# Explicit dummy values already used by the public auth regression fixtures.
DUMMIES = {'test-client', 'test-config', 'old', 'test', 'unused', 'dummy', 'KEY', 'test-private', 'test-only-local-key'}
NGINX_AUTH = re.compile(r'\banthropic_openai_api_key\s+[\"\x27]?([A-Za-z0-9_+/-]+)[\"\x27]?\s*;')
AUTH = re.compile(r'\bBearer\s+([A-Za-z0-9_.~+/-]+)')
ASSIGN = re.compile(r'(?:api[_-]?key|access[_-]?token|password|secret)\s*[=:]\s*[\"\x27]?([A-Za-z0-9_+/-]{12,})', re.I)
IP = re.compile(r'(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])')
EMAIL = re.compile(r'\b[A-Za-z0-9_.+-]+@([A-Za-z0-9.-]+\.[A-Za-z]{2,})\b')
TOP_FILES = {'Makefile', 'config', 'README.md', 'LICENSE', 'CHANGELOG.md', 'SECURITY.md', 'AGENTS.md', '.gitignore'}
TOP_DIRS = {'src', 't', 'scripts', 'conf', 'docs', '.github', 'deps', 'ci'}
MAX_BYTES = 128 * 1024 * 1024


class Scanner:
    def __init__(self):
        self.findings = set()
        self.count = 0

    def flag(self, label, rule):
        # Labels themselves may be sensitive, so suppress suspicious names too.
        if any(pattern.search(label) for pattern in RULES.values()) or EMAIL.search(label):
            label = '[redacted-path]'
        self.findings.add((label, rule))

    def content(self, label, data, depth=0):
        self.count += 1
        if len(data) > MAX_BYTES or depth > 4:
            self.flag(label, 'scan-limit')
            return
        text = data.decode('utf-8', errors='replace')
        for rule, pattern in RULES.items():
            if pattern.search(text):
                self.flag(label, rule)
        for pattern in (AUTH, ASSIGN, NGINX_AUTH):
            for match in pattern.finditer(text):
                if match.group(1) not in DUMMIES:
                    self.flag(label, 'credential-assignment')
        for match in IP.finditer(text):
            try:
                address = ipaddress.ip_address(match.group())
            except ValueError:
                continue
            if (str(address).startswith(('10.', '192.168.', '169.254.')) or address in ipaddress.ip_network((0xac100000, 12))) and not (address.is_loopback or address.is_unspecified or str(address).startswith(('192.0.2.', '198.51.100.', '203.0.113.'))):
                self.flag(label, 'private-ip')
        for match in EMAIL.finditer(text):
            if not match.group(1).endswith(('.invalid', '.example')) and match.group(1) not in {'example.com', 'example.org', 'example.net'}:
                self.flag(label, 'email-review')
        stream = io.BytesIO(data)
        try:
            if zipfile.is_zipfile(stream):
                with zipfile.ZipFile(stream) as archive:
                    for member in archive.infolist():
                        if self.member(label, member.filename, member.file_size):
                            self.content(label + ':member', archive.read(member), depth + 1)
            elif data[:2] == b'\x1f\x8b' or label.endswith(('.tar', '.tgz', '.tar.gz', '.tar.xz', '.tar.bz2')):
                stream.seek(0)
                with tarfile.open(fileobj=stream, mode='r:*') as archive:
                    for member in archive:
                        if not self.member(label, member.name, member.size):
                            continue
                        if member.issym() or member.islnk():
                            self.flag(label, 'archive-link')
                        elif member.isfile():
                            self.content(label + ':member', archive.extractfile(member).read(MAX_BYTES + 1), depth + 1)
        except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile, RuntimeError):
            self.flag(label, 'unreadable-archive')

    def member(self, label, name, size):
        parts = pathlib.PurePosixPath(name.replace('\\', '/'))
        if parts.is_absolute() or '..' in parts.parts or re.match(r'^[A-Za-z]:', name):
            self.flag(label, 'archive-path')
            return False
        self.content(label + ':member-name', name.encode())
        if size > MAX_BYTES:
            self.flag(label, 'scan-limit')
            return False
        return True

    def file(self, root, path):
        label = str(path.relative_to(root))
        self.content(label + ':name', label.encode())
        if path.is_symlink():
            self.flag(label, 'symlink-review')
            return
        try:
            with path.open('rb') as handle:
                self.content(label, handle.read(MAX_BYTES + 1))
        except OSError:
            self.flag(label, 'unreadable-file')


def git(root, *args):
    return subprocess.run(['git', '-C', str(root), *args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument('--history', action='store_true')
    parser.add_argument('--artifacts', type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    scanner = Scanner()
    own_git = (root / '.git').exists()
    try:
        tracked = git(root, 'ls-files', '-z').split(b'\0') if own_git else []
        if any(tracked):
            files = [root / p.decode() for p in tracked if p]
        else:
            files = [root / name for name in TOP_FILES if (root / name).is_file()]
            for name in TOP_DIRS:
                files.extend(p for p in (root / name).rglob('*') if p.is_file() and p.suffix not in {'.pyc', '.o', '.so'} and not re.fullmatch(r'(?:conv_test|filter_schedule)(?:.*san)?', p.name) and not {'__pycache__', '.git', 'node_modules', '.venv', 'venv', 'build', '.build', '.build-src', 'servroot'} & set(p.relative_to(root).parts))
        for path in files:
            scanner.file(root, path)
        if args.history:
            if not own_git:
                scanner.flag('git-history', 'missing-repository')
            else:
                # Includes unreachable objects: branch deletion cannot hide data.
                for line in git(root, 'cat-file', '--batch-all-objects', '--batch-check=%(objectname) %(objecttype) %(objectsize)').decode().splitlines():
                    oid, kind, size = line.split()
                    label = 'git-object:' + oid
                    if int(size) > MAX_BYTES:
                        scanner.flag(label, 'scan-limit')
                    else:
                        scanner.content(label, git(root, 'cat-file', kind, oid))
        if args.artifacts:
            artifacts = args.artifacts.resolve()
            if not artifacts.is_dir() or not any(artifacts.iterdir()):
                scanner.flag('artifacts', 'missing-artifacts')
            else:
                for path in artifacts.rglob('*'):
                    if path.is_file() or path.is_symlink():
                        scanner.file(artifacts, path)
    except (OSError, subprocess.CalledProcessError, UnicodeError, ValueError):
        scanner.flag('scan', 'incomplete-scan')
    for label, rule in sorted(scanner.findings):
        print('{}: {}'.format(label, rule))
    print('Scanned {} payloads; {} findings. Heuristic scan requires human review.'.format(scanner.count, len(scanner.findings)))
    return 1 if scanner.findings else 0


if __name__ == '__main__':
    sys.exit(main())
