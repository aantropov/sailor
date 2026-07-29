# Protobuf 5.29.3 exposes different DLL ABI and constinit declarations to
# MSVC and clang-cl. Keep consumers compatible with the MSVC-built DLL until
# the pinned vcpkg protobuf contains the upstream fix:
# https://github.com/protocolbuffers/protobuf/issues/19975#issuecomment-2769977832
set(SAILOR_PROTOBUF_PORT_DEF
    "${CURRENT_PACKAGES_DIR}/include/google/protobuf/port_def.inc")

vcpkg_replace_string(
    "${SAILOR_PROTOBUF_PORT_DEF}"
    "#if defined( __clang__) || defined(__GNUC__)"
    "#if (defined(__clang__) || defined(__GNUC__)) && (!defined(_MSC_VER) || !defined(PROTOBUF_USE_DLLS))")

vcpkg_replace_string(
    "${SAILOR_PROTOBUF_PORT_DEF}"
    "#if defined(_MSC_VER) && !defined(__clang__)"
    "#if defined(_MSC_VER) && (!defined(__clang__) || defined(PROTOBUF_USE_DLLS))")
