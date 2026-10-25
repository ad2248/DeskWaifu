"""Check the application's Qt window wiring; live protocol checks run separately."""

from pathlib import Path
import unittest
from qt_headless_smoke import assert_keyboard_capability

SOURCE = Path(__file__).resolve().parents[1] / "src"


class QtWindowConfigurationTest(unittest.TestCase):
    def test_keyboard_focus_mode_requires_protocol_evidence(self):
        for prefix in ("wl_seat@", "wl_seat#"):
            empty = prefix + "16.capabilities(0)"
            keyboard = prefix + "16.capabilities(3)"
            assert_keyboard_capability(empty, expected=False)
            assert_keyboard_capability(keyboard, expected=True)
            with self.assertRaises(AssertionError):
                assert_keyboard_capability(empty, expected=True)
            with self.assertRaises(AssertionError):
                assert_keyboard_capability(keyboard, expected=False)
        with self.assertRaises(AssertionError):
            assert_keyboard_capability("", expected=False)

    def test_screen_edit_uses_tested_frame_input_region(self):
        self.assertIn("_window.SetInputRegion(InputRegions(state,",
                      (SOURCE / "processor/interaction/InteractionProcessor.cpp").read_text())
        self.assertIn("ModelFrameInputRegions(_settings.ScreenEditing(), state.width, state.height, regions)",
                      (SOURCE / "processor/interaction/InteractionProcessor.cpp").read_text())

    def test_overlay_covers_reserved_panel_area(self):
        source = (SOURCE / "processor/interaction/window/QtDesktopWindow.cpp").read_text()
        self.assertIn("layer->setExclusiveZone(-1)", source)
        self.assertIn("layer->setLayer(LayerShellQt::Window::LayerOverlay)", source)
        for edge in ("Top", "Bottom", "Left", "Right"):
            self.assertIn("LayerShellQt::Window::Anchor" + edge, source)

    def test_qt_is_unconditional(self):
        self.assertNotIn("DESKWAIFU_QT_WINDOW", (SOURCE / "main.cpp").read_text())
        self.assertNotIn("DESKWAIFU_WINDOW_BACKEND", (SOURCE / "CMakeLists.txt").read_text())

    def test_legacy_minimum_demo_is_retired(self):
        self.assertNotIn("CSM_MINIMUM_DEMO", (SOURCE / "CMakeLists.txt").read_text())
        for name in ("mainMinimum.cpp", "CubismUserModelExtend.cpp",
                     "CubismUserModelExtend.hpp", "MouseActionManager.cpp",
                     "MouseActionManager.hpp"):
            with self.subTest(name=name):
                self.assertFalse((SOURCE / name).exists())


if __name__ == "__main__":
    unittest.main()
