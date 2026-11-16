"""Keep the Rust-only build and runner boundaries explicit."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RustCiTest(unittest.TestCase):
    def test_required_checks_are_offline_and_locked(self):
        ci = (ROOT / '.gitea/workflows/ci.yml').read_text()
        for command in ('cargo fmt --all --check',
                        'cargo clippy --workspace --all-targets --frozen -- -D warnings',
                        'cargo test --workspace --frozen',
                        'bash scripts/build.sh',
                        'CARGO_NET_OFFLINE=true bash scripts/build.sh --release'):
            self.assertIn(command, ci)
        build = (ROOT / 'scripts/build.sh').read_text()
        self.assertIn('cargo build --workspace --locked', build)
        self.assertIn('libdeskwaifu_test_plugin.so', build)
        self.assertLess(ci.index('bash scripts/build.sh'), ci.index('cargo test --workspace --frozen'))
        self.assertNotIn('runs-on: deskwaifu-arch', ci)
        self.assertNotIn('cmake ', ci)
        self.assertNotIn('pacman -S', ci)

    def test_package_acceptance_uses_independent_cli(self):
        ci = (ROOT / '.gitea/workflows/ci.yml').read_text()
        self.assertIn('/usr/bin/deskwaifucli --help', ci)
        self.assertNotIn('/usr/bin/deskwaifu --help', ci)
        self.assertIn('readlink -f /usr/bin/deskwaifu', ci)
        self.assertIn('/usr/lib/deskwaifu/plugins/libdeskwaifu_test_plugin.so', ci)

    def test_image_contains_no_private_sources(self):
        image = (ROOT / 'Containerfile').read_text()
        for legacy in ('COPY ', 'CubismSdk', 'qt6-', 'onnxruntime'):
            self.assertNotIn(legacy, image)
        script = (ROOT / 'scripts/rebuild-ci-image.sh').read_text()
        for check in ('cargo fmt', 'cargo clippy', 'cargo test', 'cargo build', 'makepkg --version'):
            self.assertIn(check, script)
        self.assertIn('deskwaifu-rust:docker://', script)

    def test_workspace_is_root_and_has_no_owned_cpp(self):
        self.assertTrue((ROOT / 'Cargo.lock').is_file())
        for directory in ('apps', 'crates', 'plugins'):
            for extension in ('*.cpp', '*.hpp', '*.c', '*.h'):
                self.assertEqual(list((ROOT / directory).rglob(extension)), [])
        self.assertFalse((ROOT / 'CMakeLists.txt').exists())
