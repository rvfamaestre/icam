"""Export an xtool signing identity locally without printing keys or passwords.

Run in WSL using a private Linux directory (not /mnt/c):
  python3 scripts/make-livecontainer-cert.py ~/icam-private/identity.p12 \
      ~/icam-private/password.txt --identity ~/.config/xtool/data/TEAM_ID
"""

import argparse
import os
from pathlib import Path
import plistlib
import secrets
import stat
import subprocess
import tempfile


def run_openssl(*args):
    result = subprocess.run(["openssl", *args], capture_output=True)
    if result.returncode:
        raise RuntimeError("OpenSSL failed; no signing material was printed.")
    return result


def private_destination(value):
    path = Path(value).expanduser()
    parent = path.parent.resolve(strict=True)
    # chmod does not reliably restrict Windows drives mounted by WSL without metadata support.
    if os.name != "posix" or parent == Path("/mnt") or Path("/mnt") in parent.parents:
        raise ValueError("Use a private Linux directory in WSL, outside /mnt.")
    info = parent.stat()
    if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
        raise ValueError("Output directories must be owned by you with permissions 0700.")
    path = parent / path.name
    if path.exists() or path.is_symlink():
        raise ValueError("Output files must not already exist.")
    return path


def write_private(path, data):
    # Exclusive creation rejects symlinks and closes the exists()/open() race.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_p12")
    parser.add_argument("password_file")
    parser.add_argument("--identity", required=True, help="xtool identity plist; no team ID is stored in this script")
    parser.add_argument("--modern", action="store_true", help="AES-256/SHA-256; verify that your iOS importer supports it")
    args = parser.parse_args()
    os.umask(0o077)
    out_p12 = private_destination(args.output_p12)
    pw_file = private_destination(args.password_file)
    if out_p12 == pw_file:
        raise ValueError("The certificate and password need different output files.")
    with Path(args.identity).expanduser().open("rb") as stream:
        identity = plistlib.load(stream)
    password = secrets.token_urlsafe(32)
    # Do not let TMPDIR redirect plaintext signing material onto a Windows/shared filesystem.
    with tempfile.TemporaryDirectory(dir=out_p12.parent) as directory:
        tmp = Path(directory)
        key, cert, pw = (tmp / name for name in ("key.pem", "cert.der", "password"))
        for path, data in ((key, identity["privateKey"]["data"]), (cert, identity["certificate"]), (pw, password.encode())):
            write_private(path, data)
        cert_pem = tmp / "cert.pem"
        write_private(cert_pem, run_openssl("x509", "-inform", "DER", "-in", str(cert)).stdout)
        package = tmp / "identity.p12"
        # Compatibility format for Apple importers. --modern opts into PBES2 after an import check.
        crypto = ["-keypbe", "AES-256-CBC", "-certpbe", "AES-256-CBC", "-macalg", "sha256"] if args.modern else [
            "-keypbe", "PBE-SHA1-3DES", "-certpbe", "PBE-SHA1-3DES", "-macalg", "sha1"]
        run_openssl("pkcs12", "-export", "-inkey", str(key), "-in", str(cert_pem), "-name", "iCam signing (xtool)",
                    *crypto, "-iter", "200000", "-passout", "file:" + str(pw), "-out", str(package))
        # Verify MAC/structure without decrypting private keys into process output.
        run_openssl("pkcs12", "-in", str(package), "-passin", "file:" + str(pw), "-info", "-noout")
        write_private(out_p12, package.read_bytes())
        try:
            write_private(pw_file, ("Contraseña del certificado de iCam para LiveContainer:\n\n" + password +
                "\n\nGuárdala en tu gestor de contraseñas. Borra las copias de transferencia después de importar.\n").encode())
        except Exception:
            out_p12.unlink()
            raise
    print("Certificate exported and verified. Private output files created with mode 0600.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, plistlib.InvalidFileException) as error:
        raise SystemExit("Export failed (" + type(error).__name__ + "). Check paths, permissions and identity format.") from None
