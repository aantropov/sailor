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
        print("Skipping path tracing smoke test: requires Windows build")
        return 0

    lib_path, binaries_root, build_type = find_engine_dll()
    if lib_path is None:
        print(f"Skipping path tracing smoke test: no Sailor-{build_type}.dll found in {binaries_root}")
        return 0

    try:
        lib = ctypes.CDLL(lib_path)
    except Exception as exc:
        print(f"Failed to load engine DLL: {exc}")
        return 1

    try:
        render_path_traced_image = lib.RenderPathTracedImage
        render_path_traced_image.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]
        render_path_traced_image.restype = ctypes.c_bool
    except Exception as exc:
        print(f"Skipping path tracing smoke test: RenderPathTracedImage export is unavailable in {lib_path} ({exc})")
        return 0

    # Empty output path should be rejected immediately and never crash.
    if render_path_traced_image(b"", b"", 64, 1, 1) is not False:
        print("Expected empty output path to return false")
        return 1

    # Without editor bootstrap this call should fail safely, not crash.
    result = render_path_traced_image(b"pathtrace_smoke.png", b"", 64, 1, 1)
    if result is not False:
        print("Expected uninitialized editor path tracing export to return false")
        return 1

    print("Path tracing smoke checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
