"""Keep the Rust-only build and runner boundaries explicit."""
from pathlib import Path
import fcntl
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RustCiTest(unittest.TestCase):
    # 测试目的：确认 CI 直接构建整个锁定的工作区，并运行原生离屏测试。
    # 测试方法：检查工作流中两个构建配置、离线依赖安装顺序和渲染命令。
    def test_required_checks_are_offline_and_locked(self):
        ci = (ROOT / '.gitea/workflows/ci.yml').read_text()
        for command in ('cargo fmt --all --check',
                        'cargo clippy --workspace --all-targets --frozen -- -D warnings',
                        'cargo test --workspace --frozen',
                        'env -u DISPLAY -u WAYLAND_DISPLAY -u CUBISM_TEST_PCI -u CUBISM_TEST_SECOND_PCI',
                        'LIBGL_ALWAYS_SOFTWARE=1 cargo test --frozen -p cubism-bridge '
                        '--features native --test offscreen -- --ignored'):
            self.assertIn(command, ci)
        builds = [line.strip() for line in ci.splitlines() if 'cargo build ' in line]
        build = 'run: bash scripts/ci-sccache.sh /cache/sccache-rust-test cargo build --workspace --frozen'
        self.assertEqual(builds, [build, f'{build} --release'])
        self.assertNotIn('scripts/build.sh', ci)
        self.assertNotIn('scripts/deploy-plugins.sh', ci)
        self.assertNotIn('cargo fetch', ci)
        self.assertIn("DESKWAIFU_CI_OFFLINE: '1'", ci)
        self.assertLess(ci.index('install -Dm644 /opt/deskwaifu-cargo/config.toml'), ci.index(build))
        self.assertLess(ci.index(build), ci.index('cargo test --workspace --frozen'))
        self.assertNotIn('runs-on: deskwaifu-arch', ci)
        self.assertNotIn('cmake ', ci)
        self.assertNotIn('pacman -S', ci)

    def test_installed_package_checks_panel_and_plugins(self):
        ci = (ROOT / '.gitea/workflows/ci.yml').read_text()
        install = ci.index('pacman -U --noconfirm release-assets/deskwaifu-bin-*.pkg.tar.zst')
        checks = ci.index('- name: Clean package artifacts', install)
        smoke = ci[install:checks]
        self.assertIn('test -x /usr/bin/deskwaifu-terminal-panel', smoke)
        for plugin in ('libdeskwaifu_test_plugin.so', 'libdeskwaifu_voice_input.so', 'libdeskwaifu_terminal.so'):
            self.assertIn(f'test -f /usr/lib/deskwaifu/plugins/{plugin}', smoke)
        self.assertIn('/usr/bin/deskwaifucli help', smoke)
        self.assertIn('^terminal[[:space:]]', smoke)

    def test_image_builds_workspace_with_native_dependencies(self):
        image = (ROOT / 'Containerfile').read_text()
        pacman_packages = image.split('pacman -Syu --noconfirm ', 1)[1].split('&& pacman -Scc', 1)[0].split()
        for package in ('cmake', 'libglvnd', 'mesa', 'systemd-libs', 'wayland', 'freetype2', 'fontconfig',
                        'libxkbcommon', 'libxkbcommon-x11', 'libxcb', 'libx11', 'vulkan-icd-loader'):
            self.assertIn(package, pacman_packages)
        script = (ROOT / 'scripts/rebuild-ci-image.sh').read_text()
        self.assertIn('--network none', script)
        self.assertIn('cargo build --workspace --frozen\n', script)
        self.assertIn('test -x "$CARGO_TARGET_DIR/debug/deskwaifu-terminal-panel"', script)
        self.assertIn('$CARGO_TARGET_DIR/debug/libdeskwaifu_terminal.so', script)
        self.assertIn('readelf -d "$terminal_plugin"', script)

    def test_image_bundles_only_selected_live2d_assets(self):
        image = (ROOT / 'Containerfile').read_text()
        self.assertNotIn('qt6-', image)
        self.assertNotIn('onnxruntime', image)
        self.assertIn('COPY Cargo.lock /tmp/workspace-Cargo.lock', image)
        self.assertIn('COPY --from=cargo-dependencies /opt/deskwaifu-cargo /opt/deskwaifu-cargo', image)
        resources = 'COPY CubismSdkForNative-5-r.5/Samples/Resources/'
        models = [line.removeprefix(resources).split('/', 1)[0]
                  for line in image.splitlines() if line.startswith(resources)]
        self.assertCountEqual(models, ['Haru', 'Hiyori', 'Mao'])
        self.assertNotIn('COPY CubismSdkForNative-5-r.5/ ', image)
        self.assertIn('cargo vendor --locked', image)
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
