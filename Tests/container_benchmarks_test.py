import ctypes
import os
import platform
import sys


def main():
    if platform.system() != "Windows":
        print("Skipping container benchmarks: requires Windows build")
        return 0

    build_type = os.environ.get("CONFIG", "Release")
    binaries_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Binaries"))
    candidate_paths = [
        os.path.join(binaries_root, build_type, f"Sailor-{build_type}.dll"),
        os.path.join(binaries_root, f"Sailor-{build_type}.dll"),
    ]

    lib_path = next((path for path in candidate_paths if os.path.exists(path)), None)
    if lib_path is None:
        print(f"Skipping container benchmarks: no Sailor-{build_type}.dll found in {binaries_root}")
        return 0

    try:
        lib = ctypes.CDLL(lib_path)
        required_functions = [
            "RunVectorBenchmark",
            "RunSetBenchmark",
            "RunMapBenchmark",
            "RunListBenchmark",
        ]

        resolved_functions = []
        for name in required_functions:
            resolved_functions.append(getattr(lib, name))

        should_run = os.environ.get("RUN_CONTAINER_BENCHMARKS", "0") == "1"
        if should_run:
            for fn in resolved_functions:
                fn()
        else:
            print("Container benchmark functions were resolved successfully; execution is skipped by default.")
    except Exception as exc:
        print(f"Benchmark execution failed: {exc}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
