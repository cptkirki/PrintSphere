"""Run production UI logic against actual LVGL with mocked display/PNG hardware."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]


def find(name, fallback):
    return shutil.which(name) or str(fallback)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=find("g++", root / "build-host/w64devkit/bin/g++.exe"))
    parser.add_argument("--cmake", default=find("cmake", Path("C:/Espressif/tools/cmake/3.30.2/bin/cmake.exe")))
    parser.add_argument("--ninja", default=find("ninja", Path("C:/Espressif/tools/ninja/1.12.1/ninja.exe")))
    args = parser.parse_args()
    cxx = Path(args.cxx).resolve()
    cc = cxx.with_name("gcc.exe" if sys.platform == "win32" else "gcc")
    build = root / "build-host/lvgl-ui"
    build.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(cxx.parent) + os.pathsep + env["PATH"]
    commands = [
        [args.cmake, "-S", str(root / "tests/lvgl_host"), "-B", str(build), "-G", "Ninja",
         "-DCMAKE_C_COMPILER=" + str(cc), "-DCMAKE_CXX_COMPILER=" + str(cxx),
         "-DCMAKE_MAKE_PROGRAM=" + args.ninja, "-DCMAKE_BUILD_TYPE=Debug"],
        [args.cmake, "--build", str(build), "--parallel", str(min(os.cpu_count() or 4, 8))],
    ]
    log = build / "test-build.log"
    with log.open("w") as output:
        for command in commands:
            result = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT)
            if result.returncode:
                output.flush()
                print("\n".join(log.read_text(errors="replace").splitlines()[-35:]))
                raise SystemExit(result.returncode)
    for board in ("amoled", "lcd"):
        binary = build / ("ui-" + board + "-tests" + (".exe" if sys.platform == "win32" else ""))
        subprocess.run([str(binary)], env=env, check=True, timeout=30)


if __name__ == "__main__":
    main()
