"""Release scanner regressions use generated, nonfunctional sensitive fixtures."""
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest

SCANNER = pathlib.Path(__file__).resolve().parents[1] / 'scripts/check-public.py'


class PublicCheck(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / 'src').mkdir()
        (self.root / 'src/clean.c').write_text('/* public */\n')

    def run_scan(self, *args):
        return subprocess.run([sys.executable, str(SCANNER), '--root', str(self.root), *args], capture_output=True, text=True)

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.root), '-c', 'user.name=Release Test', '-c', 'user.email=codex@agents.invalid', *args], check=True, capture_output=True)

    def test_untracked_downloads_excluded_before_git(self):
        download = self.root / 't/sdk/node_modules/pkg'
        download.mkdir(parents=True)
        (download / 'fixture').write_text('sk-' + 'AbCd1234' * 5)
        self.assertEqual(self.run_scan().returncode, 0)

    def test_tracked_files_cannot_use_source_exclusions(self):
        self.git('init', '-q')
        (self.root / 'unexpected.bin').write_text('sk-' + 'AbCd1234' * 5)
        self.git('add', '.')
        self.assertEqual(self.run_scan().returncode, 1)

    def test_clean_source(self):
        self.assertEqual(self.run_scan().returncode, 0)

    def test_source_secret_redacted(self):
        secret = 'sk-' + 'AbCd1234' * 5
        (self.root / 'src/bad.c').write_text(secret)
        result = self.run_scan()
        self.assertEqual(result.returncode, 1)
        self.assertIn('credential', result.stdout)
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_private_locations(self):
        for content in ['/' + 'home/' + 'private-person/code', 'http://' + '192.168.' + '44.7:9000', 'https://host.' + 'internal', 'collaboration/' + 'tasks/private.md']:
            with self.subTest(content=content):
                (self.root / 'src/bad.c').write_text(content)
                self.assertEqual(self.run_scan().returncode, 1)

    def test_deleted_history_and_unreachable_blob(self):
        self.git('init', '-q')
        self.git('add', '.')
        self.git('commit', '-qm', 'clean')
        secret = 'ghp_' + 'A1b2C3d4' * 5
        bad = self.root / 'src/removed.c'
        bad.write_text(secret)
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        bad.unlink()
        self.git('add', '-u')
        self.git('commit', '-qm', 'remove')
        self.assertEqual(self.run_scan().returncode, 0)
        self.assertEqual(self.run_scan('--history').returncode, 1)
        self.git('update-ref', '-d', 'refs/heads/master') if self.git('branch', '--show-current').stdout.strip() == b'master' else self.git('update-ref', '-d', 'refs/heads/main')
        result = self.run_scan('--history')
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_binary_and_archive(self):
        artifacts = self.root / 'release'
        artifacts.mkdir()
        binary = artifacts / 'module.so'
        binary.write_bytes(b'\x7fELF\x00' + ('/' + 'home/' + 'private-person/source.c').encode())
        self.assertEqual(self.run_scan('--artifacts', str(artifacts)).returncode, 1)
        binary.unlink()
        with tarfile.open(artifacts / 'source.tar.gz', 'w:gz') as archive:
            info = tarfile.TarInfo('../escaped.c')
            info.size = 2
            archive.addfile(info, io.BytesIO(b'ok'))
        self.assertEqual(self.run_scan('--artifacts', str(artifacts)).returncode, 1)

    def test_history_requires_own_repository(self):
        self.assertNotEqual(self.run_scan('--history').returncode, 0)

    def test_nginx_literal_credential(self):
        for quote in ['', chr(34), chr(39)]:
            (self.root / 'src/bad.c').write_text('anthropic_openai_api_key ' + quote + 'realLookingCredential123' + quote + ';')
            self.assertEqual(self.run_scan().returncode, 1)

    def test_archive_secret(self):
        artifacts = self.root / 'release'
        artifacts.mkdir()
        secret = ('sk-' + 'aBcDeFgH1234' * 4).encode()
        with tarfile.open(artifacts / 'source.tar.gz', 'w:gz') as archive:
            info = tarfile.TarInfo('public/file.c')
            info.size = len(secret)
            archive.addfile(info, io.BytesIO(secret))
        result = self.run_scan('--artifacts', str(artifacts))
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(secret.decode(), result.stdout + result.stderr)

    def test_history_private_author(self):
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.email=' + 'person@' + 'private-company.test', 'commit', '-qm', 'fixture')
        self.assertEqual(self.run_scan('--history').returncode, 1)

    def test_fixture_exemption_is_narrow(self):
        (self.root / 'src/bad.c').write_text('Authorization: Bearer ' + 'randomCredential123456789')
        self.assertEqual(self.run_scan().returncode, 1)


if __name__ == '__main__':
    unittest.main()
