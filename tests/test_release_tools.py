import importlib.util
from pathlib import Path
import struct
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packager = load("packager", "tools/package_initial_flash.py")
patcher = load("patcher", "tools/patches/apply_adapter_patches.py")


class ReleaseTests(unittest.TestCase):
    def image(self, project):
        data = bytearray(288)
        data[0] = 0xE9
        struct.pack_into("<I", data, 32, 0xABCD5432)
        data[48:54] = b"v1.7.0"
        data[80:80 + len(project)] = project.encode()
        return bytes(data)

    def test_board_identity(self):
        self.assertEqual(packager.app_identity(self.image("printsphere_2_8c_p2"))["board"], "lcd_2_8c")
        self.assertEqual(packager.app_identity(self.image("printsphere_1_75_p2"))["board"], "amoled_1_75")

    def test_unidentified_or_broken_image_rejected(self):
        for data in (b"invalid", self.image("printsphere_idf"), self.image("another_project")):
            with self.assertRaises(ValueError):
                packager.app_identity(data)

    def test_source_hash_ignores_generated_python_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("VERSION", "CMakeLists.txt", "dependencies.lock", "sdkconfig.defaults",
                         "partitions.csv", "tools/package_initial_flash.py", "main/example.cpp"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("source")
            before = packager.source_tree_hash(root)
            cache = root / "tools/patches/__pycache__/patcher.pyc"
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"generated cache")
            self.assertEqual(before, packager.source_tree_hash(root))
            (root / "main/example.cpp").write_text("changed source")
            self.assertNotEqual(before, packager.source_tree_hash(root))

    def test_patch_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "src/display/bridge/v9/lvgl_bridge_v9.c"
            path.parent.mkdir(parents=True)
            # Use the pinned component to cover the full patch context.
            source = ROOT / "managed_components/espressif__esp_lvgl_adapter/src/display/bridge/v9/lvgl_bridge_v9.c"
            text = source.read_text()
            # Strip the known patched sections so a first application is tested.
            text = text.replace(patcher.notifyReplace, patcher.notifyFind)
            text = text.replace(patcher.clearReplace, patcher.clearFind)
            text = text.replace("ulTaskNotifyTakeIndexed(1, pdTRUE, timeout)", "ulTaskNotifyTake(pdTRUE, timeout)")
            text = text.replace(patcher.helperReplace, patcher.helperFind)
            text = re.sub(r"(?m)^ *PRINTSPHERE_DISPLAY_TRACE\(\d+\);\n", "", text)
            text = text.replace(patcher.traceDeclaration, "")
            text = text.replace(patcher.gpioReplace, patcher.gpioFind)
            self.assertNotIn("display_bridge_v9_wait_tx_done",text)
            self.assertIn(patcher.gpioFind,text)
            path.write_text(text)
            patcher.apply(directory)
            once = path.read_bytes()
            patcher.apply(directory)
            self.assertEqual(once, path.read_bytes())
            patched=path.read_text()
            self.assertIn(patcher.notifyReplace,patched)
            self.assertIn(patcher.clearReplace,patched)
            self.assertIn("ulTaskNotifyTakeIndexed(1, pdTRUE, timeout)",patched)

    def test_patch_source_drift_is_a_build_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "src/display/bridge/v9/lvgl_bridge_v9.c"
            path.parent.mkdir(parents=True)
            path.write_text("different upstream code")
            with self.assertRaises(RuntimeError):
                patcher.apply(directory)
