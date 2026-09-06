import importlib.util
import io
import pathlib
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BuildSafety(unittest.TestCase):
    def test_unpinned_version_rejected(self):
        with self.assertRaises(ValueError):
            load('build').version_entry('0.0.0')

    def test_checksum_rejected_before_extract(self):
        with self.assertRaises(ValueError):
            load('build').verify_bytes(b'wrong archive', '0' * 64)

    def test_archive_escape_rejected(self):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as archive:
            entry = tarfile.TarInfo('../escape')
            entry.size = 1
            archive.addfile(entry, io.BytesIO(b'x'))
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                load('build').extract_source(buf.getvalue(), pathlib.Path(directory), 'nginx-test')

    def test_compat_rejects_new_glibc(self):
        with self.assertRaises(ValueError):
            load('package').check_glibc(['2.17', '2.28'], 'compat')

    def test_glibc_order_is_numeric(self):
        self.assertEqual(load('package').check_glibc(['2.9', '2.17'], 'compat'), '2.17')


if __name__ == '__main__':
    unittest.main()
