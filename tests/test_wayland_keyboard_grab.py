"""Check keyboard and full-output geometry setup for each Wayland layer surface."""

from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src/LAppWayland.cpp"


class WaylandKeyboardGrabTest(unittest.TestCase):
    def test_each_created_layer_covers_reserved_panel_area(self):
        source = SOURCE.read_text()
        creations = list(re.finditer(r"\bzwlr_layer_shell_v1_get_layer_surface\s*\(", source))
        self.assertEqual(len(creations), 2)
        for creation in creations:
            with self.subTest(creation=source.count("\n", 0, creation.start()) + 1):
                until_first_commit = source[creation.end():].split("wl_surface_commit", 1)[0]
                self.assertRegex(
                    until_first_commit,
                    r"\bzwlr_layer_surface_v1_set_exclusive_zone\s*\(\s*(?:wl->layer_surface|layer)\s*,\s*-1\s*\)",
                )

    def test_each_created_layer_uses_shared_interactivity(self):
        source = SOURCE.read_text()
        helper = re.search(
            r"static void (\w+)\([^)]*\)\s*\{[^}]*"
            r"zwlr_layer_surface_v1_set_keyboard_interactivity\s*\(",
            source,
        )
        self.assertIsNotNone(helper)
        direct_calls = list(re.finditer(r"\bzwlr_layer_surface_v1_set_keyboard_interactivity\s*\(", source))
        self.assertTrue(all(helper.start() <= call.start() < helper.end() for call in direct_calls))

        creations = list(re.finditer(r"\bzwlr_layer_shell_v1_get_layer_surface\s*\(", source))
        self.assertTrue(creations)
        for creation in creations:
            with self.subTest(creation=source.count("\n", 0, creation.start()) + 1):
                until_first_commit = source[creation.end():].split("wl_surface_commit", 1)[0]
                self.assertRegex(until_first_commit, rf"\b{helper.group(1)}\s*\(")


if __name__ == "__main__":
    unittest.main()
