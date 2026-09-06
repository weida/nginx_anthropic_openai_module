#!/usr/bin/env python3
"""Build a pinned nginx and dynamic module; no install or service changes."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MODULE = 'ngx_http_anthropic_openai_module.so'


def version_entry(version):
    manifest = json.loads((ROOT / 'ci/versions.json').read_text())
    for entry in manifest['nginx']:
        if entry['version'] == version:
            return entry
    raise ValueError('nginx version is not pinned in ci/versions.json')


def verify_bytes(data, expected):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError('source archive SHA-256 mismatch')


def extract_source(data, destination, directory):
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive.getmembers():
            parts = Path(member.name).parts
            if (not parts or parts[0] != directory or '..' in parts
                    or member.name.startswith('/')
                    or not (member.isfile() or member.isdir())):
                raise ValueError('unsafe source archive member')
        archive.extractall(destination)


def run(command, **kwargs):
    subprocess.run(command, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nginx-version', default='1.26.3')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    entry = version_entry(args.nginx_version)
    sources = ROOT / '.build-src'
    sources.mkdir(exist_ok=True)
    directory = 'nginx-' + entry['version']
    archive_path = sources / (directory + '.tar.gz')
    if not archive_path.exists():
        with urllib.request.urlopen('https://nginx.org/download/' + archive_path.name, timeout=120) as response:
            data = response.read()
        verify_bytes(data, entry['sha256'])
        archive_path.write_bytes(data)
    data = archive_path.read_bytes()
    verify_bytes(data, entry['sha256'])
    # Re-extract checked sources, rather than trusting a previous configure tree.
    source = sources / directory
    if source.exists():
        shutil.rmtree(source)
    extract_source(data, sources, directory)
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    flags = [
        '--prefix=/opt/nginx-module-test', '--builddir=' + str(build),
        '--with-compat', '--with-stream', '--without-http_gzip_module',
        '--add-dynamic-module=' + str(ROOT),
        '--with-cc-opt=-O2 -ffile-prefix-map=' + str(ROOT) + '=/work/public-module',
    ]
    if Path('/usr/local/include/pcre2.h').exists():
        flags[-1] += ' -I/usr/local/include'
        flags.append('--with-ld-opt=-L/usr/local/lib')
    run(['./configure'] + flags, cwd=source)
    run(['make', '-f', str(build / 'Makefile'), '-j', str(args.jobs)], cwd=source)
    run(['strip', '--strip-unneeded', str(build / MODULE)])
    # A real load check in the build environment proves the runtime baseline.
    with tempfile.TemporaryDirectory(prefix='nginx-module-smoke-') as prefix:
        config = Path(prefix) / 'nginx.conf'
        config.write_text('load_module ' + str(build / MODULE) + ';\n'
                          'error_log stderr;\npid ' + prefix + '/nginx.pid;\n'
                          'events {}\nhttp { access_log off; }\n')
        run([str(build / 'nginx'), '-t', '-p', prefix + '/', '-c', str(config)])
    compiler = subprocess.check_output([os.environ.get('CC', 'cc'), '--version'], text=True).splitlines()[0]
    info = {'schema_version': 1, 'nginx_version': entry['version'],
            'nginx_source_sha256': entry['sha256'], 'compiler': compiler,
            'configure_arguments': [arg.replace(str(ROOT), '/work/public-module') for arg in flags],
            'baseline_load_test': 'passed'}
    (build / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')


if __name__ == '__main__':
    main()
