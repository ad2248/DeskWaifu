"""Keep the Rust-only build and runner boundaries explicit."""
from pathlib import Path
import fcntl
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RustCiTest(unittest.TestCase):
    # 测试目的：确认 CI 保持锁定、离线构建约定，并在测试前部署两个插件。
    # 测试方法：检查工作流和构建脚本中的命令、选项、插件名及步骤顺序。
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
        for plugin in ('libdeskwaifu_test_plugin.so', 'libdeskwaifu_voice_input.so'):
            self.assertIn(plugin, build)
        self.assertLess(ci.index('cargo fetch --locked'), ci.index('CARGO_NET_OFFLINE=true bash scripts/build.sh'))
        self.assertLess(ci.index('bash scripts/build.sh'), ci.index('cargo test --workspace --frozen'))
        self.assertNotIn('runs-on: deskwaifu-arch', ci)
        self.assertNotIn('cmake ', ci)
        self.assertNotIn('pacman -S', ci)

    # 测试目的：确认 CI 的安装验收会检查独立 CLI 和语音命令的发现。
    # 测试方法：检查工作流是否核对安装路径，并用已安装的 CLI 查询 help 输出中的语音启动命令。
    def test_package_acceptance_uses_independent_cli(self):
        ci = (ROOT / '.gitea/workflows/ci.yml').read_text()
        self.assertIn('/usr/bin/deskwaifucli --help', ci)
        self.assertNotIn('/usr/bin/deskwaifu --help', ci)
        self.assertIn('readlink -f /usr/bin/deskwaifu', ci)
        self.assertIn('/usr/lib/deskwaifu/plugins/libdeskwaifu_test_plugin.so', ci)
        self.assertIn('/usr/lib/deskwaifu/plugins/libdeskwaifu_voice_input.so', ci)
        self.assertIn('/usr/bin/deskwaifucli help', ci)
        self.assertIn('^start_voice_input', ci)

    def test_image_contains_no_private_sources(self):
        image = (ROOT / 'Containerfile').read_text()
        for legacy in ('COPY ', 'CubismSdk', 'qt6-', 'onnxruntime'):
            self.assertNotIn(legacy, image)
        script = (ROOT / 'scripts/rebuild-ci-image.sh').read_text()
        for check in ('cargo fmt', 'cargo clippy', 'cargo test', 'cargo build', 'makepkg --version'):
            self.assertIn(check, script)
        self.assertIn('deskwaifu-rust:docker://', script)

    # 测试目的：防止两个并行作业启动访问同一磁盘缓存的 sccache 服务。
    # 测试方法：先持有目录锁，再执行缓存入口并确认构建照常运行且没有启动服务。
    def test_busy_sccache_runs_build_without_starting_server(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sccache = root / 'sccache'
            sccache.write_text('#!/bin/sh\necho unexpected-sccache-start\nexit 1\n')
            sccache.chmod(0o755)
            env = dict(os.environ, PATH=f'{root}{os.pathsep}{os.environ["PATH"]}')
            with (root / '.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                result = subprocess.run(
                    ['bash', str(ROOT / 'scripts/ci-sccache.sh'), str(root), 'echo', 'build-ran'],
                    env=env, capture_output=True, text=True, check=True, timeout=5,
                )
            self.assertIn('cache busy', result.stdout)
            self.assertIn('build-ran', result.stdout)
            self.assertNotIn('unexpected-sccache-start', result.stdout)

    # 测试目的：缓存淘汰不能删除另一个作业仍持有的目录锁。
    # 测试方法：模拟服务在启动后淘汰缓存条目，再在第一个作业结束前启动第二个作业。
    def test_sccache_eviction_cannot_remove_active_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / 'cache'
            sccache = root / 'sccache'
            sccache.write_text(
                '#!/bin/sh\n'
                'if [ "$1" = --start-server ]; then\n'
                '  echo server-started\n'
                '  rm -f -- "$SCCACHE_DIR/.lock"\n'
                'fi\n'
            )
            sccache.chmod(0o755)
            env = dict(os.environ, PATH=f'{root}{os.pathsep}{os.environ["PATH"]}')
            wrapper = ['bash', str(ROOT / 'scripts/ci-sccache.sh'), str(cache)]
            result = subprocess.run(
                [*wrapper, *wrapper, 'echo', 'second-build-ran'],
                env=env, capture_output=True, text=True, check=True, timeout=5,
            )
            self.assertEqual(result.stdout.count('server-started'), 1, result.stdout)
            self.assertIn('cache busy', result.stdout)
            self.assertIn('second-build-ran', result.stdout)

    def test_workspace_is_root_and_has_no_owned_cpp(self):
        self.assertTrue((ROOT / 'Cargo.lock').is_file())
        for directory in ('apps', 'crates', 'plugins'):
            for extension in ('*.cpp', '*.hpp', '*.c', '*.h'):
                self.assertEqual(list((ROOT / directory).rglob(extension)), [])
        self.assertFalse((ROOT / 'CMakeLists.txt').exists())
