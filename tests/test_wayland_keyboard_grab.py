"""Ensure Wayland focus callbacks and all layer surfaces use shared grab state."""

from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src/LAppWayland.cpp"


class WaylandKeyboardGrabTest(unittest.TestCase):
    def test_focus_loss_and_seat_loss_release_grab(self):
        source = SOURCE.read_text()
        self.assertRegex(source, r"static void keyboard_leave\([^)]*\)\s*\{[^}]*SetModelKeyboardGrab\([^;]*false\)")
        self.assertRegex(source, r"WL_SEAT_CAPABILITY_KEYBOARD\) && wl->keyboard\) \{\s*SetModelKeyboardGrab\(wl, false\)")

    def test_every_layer_surface_uses_shared_interactivity(self):
        source = SOURCE.read_text()
        self.assertEqual(source.count("zwlr_layer_shell_v1_get_layer_surface("), 2)
        self.assertEqual(source.count("ApplyLayerKeyboardInteractivity(wl);"), 2)
        self.assertIn("ApplyLayerKeyboardInteractivity(g_wl);", source)
        self.assertEqual(len(re.findall(r"zwlr_layer_surface_v1_set_keyboard_interactivity\(", source)), 1)


if __name__ == "__main__":
    unittest.main()
