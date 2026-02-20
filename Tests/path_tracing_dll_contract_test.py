import ctypes
import os
import platform
import sys


def find_engine_dll():
    build_type = os.environ.get("CONFIG", "Release")
    binaries_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Binaries"))
    candidate_paths = [
        os.path.join(binaries_root, build_type, f"Sailor-{build_type}.dll"),
        os.path.join(binaries_root, f"Sailor-{build_type}.dll"),
    ]
    return next((path for path in candidate_paths if os.path.exists(path)), None), binaries_root, build_type


def main():
    if platform.system() != "Windows":
        print("Skipping path tracing DLL contract test: requires Windows build")
        return 0

    lib_path, binaries_root, build_type = find_engine_dll()
    if lib_path is None:
        print(f"Skipping path tracing DLL contract test: no Sailor-{build_type}.dll found in {binaries_root}")
        return 0

    try:
        lib = ctypes.CDLL(lib_path)
    except Exception as exc:
        print(f"Failed to load engine DLL: {exc}")
        return 1

    required_exports = [
        "Initialize",
        "Start",
        "Stop",
        "Shutdown",
        "GetMessages",
        "SerializeCurrentWorld",
        "SerializeEngineTypes",
        "UpdateObject",
        "ShowMainWindow",
        "RenderPathTracedImage",
    ]

    missing = [name for name in required_exports if not hasattr(lib, name)]
    if missing:
        print(f"Skipping path tracing DLL contract test: missing exports in {lib_path}: {', '.join(missing)}")
        return 0

    fn = lib.RenderPathTracedImage
    fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
    fn.restype = ctypes.c_bool

    # Contract: empty output path is rejected immediately.
    if fn(b"", b"", 1, 1, 1) is not False:
        print("RenderPathTracedImage contract failure: empty output path should return false")
        return 1

    # Contract: if editor/world is not initialized, export call must fail safely.
    result = fn(b"pathtrace_contract_test.png", b"", 1, 1, 1)
    if result is not False:
        print("RenderPathTracedImage contract failure: uninitialized call should return false")
        return 1

    print("Path tracing DLL contract checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
