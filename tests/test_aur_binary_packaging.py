"""Exercise the AUR binary recipe against a local release archive."""

import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RustConsumerTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("bsdtar"), "bsdtar required")
    def test_consumer_preserves_binary_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "package"
            output.mkdir()
            archive = root / "deskwaifu-bin-0.1.0-1-x86_64.pkg.tar.zst"
            with tarfile.open(archive, "w") as stream:
                entry = tarfile.TarInfo("usr/bin/deskwaifu")
                entry.mode = 0o755
                entry.size = 6
                stream.addfile(entry, io.BytesIO(b"binary"))
            subprocess.run(["bash", "-euc", 'source "$1"; srcdir=$2; pkgdir=$3; package', "bash",
                            str(ROOT / "packaging/aur-template/PKGBUILD"), str(root), str(output)], check=True)
            self.assertEqual((output / "usr/bin/deskwaifu").stat().st_mode & 0o777, 0o755)


if __name__ == "__main__":
    unittest.main()
