# Reuse the port pinned by the vcpkg submodule, then apply Sailor's temporary
# consumer-header compatibility fix. Patch files referenced by the upstream
# port are mirrored beside this wrapper because vcpkg resolves them relative
# to CURRENT_PORT_DIR.
include("${VCPKG_ROOT_DIR}/ports/protobuf/portfile.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/protobuf-clang-cl-compat.cmake")
