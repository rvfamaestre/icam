"""Run in WSL: python3 -m unittest discover -s scripts/tests -v.

Only a temporary self-signed test identity is used; no real xtool signing key is read.
"""
import importlib.util
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "make-livecontainer-cert.py"
spec = importlib.util.spec_from_file_location("certificate_export", SCRIPT)
export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export)


class CertificateExportTests(unittest.TestCase):
    def test_refuses_public_directory_and_existing_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            folder.chmod(0o755)
            with self.assertRaises(ValueError):
                export.private_destination(folder / "identity.p12")
            folder.chmod(0o700)
            target = folder / "keep.txt"
            target.write_text("keep")
            link = folder / "link.p12"
            link.symlink_to(target)
            with self.assertRaises(ValueError):
                export.private_destination(link)
            with self.assertRaises(FileExistsError):
                export.write_private(link, b"replace")
            self.assertEqual(target.read_text(), "keep")

    def test_refuses_windows_mount(self):
        if Path("/mnt/c").is_dir():
            with self.assertRaises(ValueError):
                export.private_destination("/mnt/c/test-icam-export.p12")

    def test_synthetic_export_is_verified_and_private(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            key, cert = folder / "test.key", folder / "test.der"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-noenc", "-days", "1",
                            "-subj", "/CN=iCam audit test only", "-keyout", str(key), "-outform", "DER", "-out", str(cert)],
                           check=True, capture_output=True)
            identity = folder / "test.plist"
            identity.write_bytes(plistlib.dumps({"privateKey": {"data": key.read_bytes()}, "certificate": cert.read_bytes()}))
            for modern in (False, True):
                with self.subTest(modern=modern):
                    suffix = "modern" if modern else "compat"
                    package, password_file = folder / (suffix + ".p12"), folder / (suffix + "-password.txt")
                    args = ["python3", str(SCRIPT), str(package), str(password_file), "--identity", str(identity)]
                    if modern:
                        args.append("--modern")
                    result = subprocess.run(args, check=True, capture_output=True, text=True)
                    password = password_file.read_text().splitlines()[2]
                    self.assertNotIn(password, result.stdout + result.stderr)
                    self.assertEqual(package.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(password_file.stat().st_mode & 0o777, 0o600)
                    self.assertGreater(package.stat().st_size, 1000)
                    retry = subprocess.run(args, capture_output=True)
                    self.assertNotEqual(retry.returncode, 0)


if __name__ == "__main__":
    unittest.main()
