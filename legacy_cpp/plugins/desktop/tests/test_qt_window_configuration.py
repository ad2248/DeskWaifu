"""Check the application's Qt window wiring; live protocol checks run separately."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "plugins/desktop/private"


class QtWindowConfigurationTest(unittest.TestCase):
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
        self.assertNotIn("DESKWAIFU_QT_WINDOW", (ROOT / "platform/main.cpp").read_text())
        self.assertNotIn("DESKWAIFU_WINDOW_BACKEND", (SOURCE.parent / "CMakeLists.txt").read_text())

if __name__ == "__main__":
    unittest.main()
