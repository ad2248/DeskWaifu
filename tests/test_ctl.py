"""Exercise the installed-style CLI against an isolated real Unix socket."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest

CLI = Path(__file__).resolve().parents[1] / "deskwaifu-ctl"


class CliTest(unittest.TestCase):
    def exchange(self, args, reply=b'{"ok":true}\n', keep_open=False):
        with tempfile.TemporaryDirectory(prefix="ctl-", dir=os.environ.get("TMPDIR")) as root:
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(Path(root) / "deskwaifu.sock"))
                server.listen(1)
                server.settimeout(.1)
                received = []
                errors = []
                finished = threading.Event()

                def serve():
                    while not finished.is_set():
                        try:
                            connection, _ = server.accept()
                        except socket.timeout:
                            continue
                        try:
                            with connection:
                                connection.settimeout(3)
                                with connection.makefile("rb") as stream:
                                    received.append(stream.readline())
                                connection.sendall(reply)
                                if keep_open:
                                    finished.wait(8)
                        except OSError as error:
                            errors.append(error)
                        return

                worker = threading.Thread(target=serve)
                worker.start()
                try:
                    result = subprocess.run([str(CLI), *args], env=dict(os.environ, XDG_RUNTIME_DIR=root),
                                            capture_output=True, text=True, timeout=8)
                finally:
                    finished.set()
                    worker.join(4)
                self.assertFalse(worker.is_alive())
                self.assertEqual(errors, [])
                return result, received

    def test_string_encoding(self):
        for name in ['夕颜', 'A"B', 'A\\B', 'A\nB', '001', '1e3']:
            with self.subTest(name=name):
                result, wire = self.exchange(["set_model", "--name", name])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(wire), 1)
                self.assertEqual(wire[0].count(b"\n"), 1)
                self.assertEqual(json.loads(wire[0]), {"command": "set_model", "name": name})

    def test_parameter_types(self):
        for args, expected in [
            (["do_motion", "--index", "2", "--priority", "3"], {"index": 2, "priority": 3}),
            (["set_model_position", "--x", "-0.25", "--y", "1e-2"], {"x": -.25, "y": .01}),
            (["set_look", "--reset"], {"reset": True}),
            (["set_hover_opacity", "--enabled", "false"], {"enabled": False}),
            (["begin_transform_edit", "--screen", "true", "--expected_revision", "2"],
             {"screen": True, "expected_revision": 2}),
        ]:
            with self.subTest(args=args):
                result, wire = self.exchange(args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(wire[0]), dict(command=args[0], **expected))

    def test_response_status(self):
        for reply in [b'{"ok":false,"error":"missing model"}\n', b'', b'broken\n',
                      b'{"ok":true', b'{"ok":true}', b'[]\n', b'{}\n', b'{"ok":1}\n',
                      b'{"ok":true,"ok":false}\n', b'{"ok":true,"value":NaN}\n',
                      b'{"ok":true,"value":"\xff"}\n']:
            with self.subTest(reply=reply):
                result, _ = self.exchange(["get_status"], reply)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                if b'missing model' in reply:
                    self.assertIn('missing model', result.stdout)

    def test_response_does_not_require_eof(self):
        result, _ = self.exchange(["get_status"], keep_open=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_response_timeout(self):
        result, _ = self.exchange(["get_status"], reply=b'', keep_open=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("timed out", result.stderr)

    def test_invalid_arguments(self):
        # 监听服务端用于证明拒绝发生在发送之前。
        for args in [["set_model", "--index", "0.9"], ["set_model", "--index", "banana"],
                     ["set_model", "--index", "2147483648"], ["set_model_zoom", "--value", "NaN"],
                     ["set_model_zoom", "--value", "1e999"], ["set_model", "--index", "001"],
                     ["set_model", "--name"], ["get_status", "oops", "value"],
                     ["get_status", "--command", "quit"]]:
            result, wire = self.exchange(args)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(wire, [])

    def test_communication_failure(self):
        with tempfile.TemporaryDirectory(prefix="ctl-") as root:
            result = subprocess.run([str(CLI), "get_status"], capture_output=True, text=True, timeout=8,
                                    env=dict(os.environ, XDG_RUNTIME_DIR=root))
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
