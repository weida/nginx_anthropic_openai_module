#!/usr/bin/env python3
"""Package a tested module with architecture, libc and source identities."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
MODULE = 'ngx_http_anthropic_openai_module.so'


def check_glibc(versions, variant):
    ceiling = {'standard': (2, 39), 'compat': (2, 17)}[variant]
    parsed = [tuple(map(int, version.split('.'))) for version in versions]
    required = max(parsed, default=(0, 0))
    if required > ceiling:
        raise ValueError('module GLIBC requirement exceeds declared variant')
    return '.'.join(map(str, required))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nginx-version', required=True)
    parser.add_argument('--variant', choices=['standard', 'compat'], required=True)
    parser.add_argument('--arch', choices=['amd64', 'arm64'], required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--build-image', required=True)
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{40}', args.source_commit):
        raise ValueError('a full public Git commit is required')
    info = json.loads((ROOT / 'build/build-info.json').read_text())
    if info['nginx_version'] != args.nginx_version or info['baseline_load_test'] != 'passed':
        raise ValueError('build identity or baseline load check mismatch')
    module = ROOT / 'build' / MODULE
    headers = subprocess.check_output(['readelf', '-h', str(module)], text=True)
    machine = {'amd64': 'Advanced Micro Devices X86-64', 'arm64': 'AArch64'}[args.arch]
    if machine not in headers:
        raise ValueError('ELF architecture mismatch')
    symbols = subprocess.check_output(['readelf', '--version-info', str(module)], text=True)
    required = check_glibc(re.findall(r'GLIBC_([0-9.]+)', symbols), args.variant)
    digest = hashlib.sha256(module.read_bytes()).hexdigest()
    info.update({'source_commit': args.source_commit, 'architecture': args.arch,
                 'variant': args.variant, 'build_image': args.build_image,
                 'module_sha256': digest, 'glibc_required': required,
                 'compatibility': 'Exact nginx version and compatible build signature required; glibc only is insufficient.'})
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    stem = 'ngx_http_anthropic_openai_module-nginx-' + args.nginx_version + '-linux-' + args.arch + '-' + args.variant
    shutil.copyfile(module, dist / (stem + '.so'))
    metadata = (json.dumps(info, indent=2, sort_keys=True) + '\n').encode()
    (dist / (stem + '.json')).write_bytes(metadata)
    with tarfile.open(dist / (stem + '.tar.gz'), 'w:gz') as archive:
        for name, data in [(MODULE, module.read_bytes()), ('build-info.json', metadata),
                           ('LICENSE', (ROOT / 'LICENSE').read_bytes()),
                           ('cJSON-LICENSE', (ROOT / 'deps/cJSON/LICENSE').read_bytes())]:
            member = tarfile.TarInfo(name)
            member.mode = 0o644
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    print(stem)


if __name__ == '__main__':
    main()
