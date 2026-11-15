"""Run production C++ policy/protocol/persistence regressions on the host."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("g++") or str(root / "build-host/w64devkit/bin/g++.exe"))
    args = parser.parse_args()
    cxx = Path(args.cxx).resolve()
    cc = cxx.with_name("gcc.exe" if sys.platform == "win32" else "gcc")
    build = root / "build-host/tests"
    build.mkdir(parents=True, exist_ok=True)
    cjson = root / "managed_components/espressif__cjson/cJSON"
    if not (cjson / "cJSON.c").is_file():
        raise SystemExit("Run idf.py reconfigure to resolve the pinned cJSON component first")
    obj = build / "cjson.o"
    subprocess.run([str(cc), "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    for name, sources, extra in [
        ("policy-tests", [root / "tests/native_tests.cpp"], []),
        ("display-power-tests", [root / "tests/display_power_tests.cpp"], []),
        ("ui-runtime-tests", [root / "tests/ui_runtime_tests.cpp"], []),
        ("amoled-dma-tests", [root / "tests/amoled_dma_tests.cpp"], []),
        ("config-tests", [root / "tests/config_store_tests.cpp", root / "main/src/config_store.cpp"], ["-I" + str(root / "tests/stubs")]),
    ]:
        binary = build / (name + (".exe" if sys.platform == "win32" else ""))
        command = [str(cxx), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", *extra,
                   "-I" + str(root / "main/include"), "-I" + str(cjson),
                   *map(str, sources), str(obj), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"], cwd=root, check=True)


if __name__ == "__main__":
    main()
