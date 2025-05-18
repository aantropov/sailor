import ctypes
import os
import platform
import sys


def main():
    if platform.system() != "Windows":
        print("Skipping container benchmarks: requires Windows build")
        return 0

    build_type = os.environ.get("CONFIG", "Release")
    binaries_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Binaries"))
    lib_path = os.path.join(binaries_dir, f"Sailor-{build_type}.dll")

    if not os.path.exists(lib_path):
        print(f"Skipping container benchmarks: {lib_path} not found")
        return 0

    try:
        lib = ctypes.CDLL(lib_path)
        lib.RunVectorBenchmark()
        lib.RunSetBenchmark()
        lib.RunMapBenchmark()
        lib.RunListBenchmark()
    except Exception as exc:
        print(f"Benchmark execution failed: {exc}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
