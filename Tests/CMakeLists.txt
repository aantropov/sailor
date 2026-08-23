find_package(ixwebsocket CONFIG QUIET)

if(NOT TARGET ixwebsocket::ixwebsocket)
    add_library(ixwebsocket::ixwebsocket INTERFACE IMPORTED)
endif()

if(WIN32)
    find_package(VulkanLoader CONFIG REQUIRED)

    function(sailor_stage_workspace_test_runtime target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}>
                $<TARGET_FILE:Vulkan::Loader>
                $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endfunction()
endif()

add_executable(RemoteViewportFoundationTests
    RemoteViewportFoundationTests.cpp
)

add_executable(RemoteViewportContractTests
    RemoteViewportContractTests.cpp
)

add_executable(RemoteViewportRuntimeTests
    RemoteViewportRuntimeTests.cpp
)

add_executable(EditorEngineProtocolTests
    EditorEngineProtocolTests.cpp
)

# Keep this executable wire-only. Compiling editor_engine.pb.cc here as well
# as inside SailorLib registers the same descriptors twice when both modules
# share the dynamic protobuf runtime on Windows.
add_executable(EditorEngineWebSocketServerTests
    EditorEngineWebSocketServerTests.cpp
)

add_executable(EditorViewportSessionTests
    EditorViewportSessionTests.cpp
)

add_executable(SmartPointerTests
    SmartPointerTests.cpp
)

add_executable(InstanceIdContractTests
    InstanceIdContractTests.cpp
)

add_executable(EcsLifecycleContractTests
    EcsLifecycleContractTests.cpp
)

add_executable(BoundsContractTests
    BoundsContractTests.cpp
)

add_executable(GpuCullingContractTests
    GpuCullingContractTests.cpp
)

add_executable(RHISceneVirtualizationTests
    RHISceneVirtualizationTests.cpp
)

add_executable(ModelImporterContractTests
    ModelImporterContractTests.cpp
)

add_executable(AnimationSamplingContractTests
    AnimationSamplingContractTests.cpp
)

add_executable(VulkanDescriptorCacheTests
    VulkanDescriptorCacheTests.cpp
)

add_executable(EditorViewportControllerContractTests
    EditorViewportControllerContractTests.cpp
)

add_executable(SkyComponentContractTests
    SkyComponentContractTests.cpp
)

add_executable(PhysicsRuntimeTests
    PhysicsRuntimeTests.cpp
)

add_executable(AudioContractTests
    AudioContractTests.cpp
)

add_executable(GraphicsSettingsTests
    GraphicsSettingsTests.cpp
)

add_library(WorkspaceModuleFixture SHARED
    WorkspaceModuleFixture.cpp
)

add_library(WorkspaceModuleIncompatibleFixture SHARED
    WorkspaceModuleIncompatibleFixture.cpp
)

add_library(WorkspaceModuleMissingEntryFixture SHARED
    WorkspaceModuleMissingEntryFixture.cpp
)

function(add_workspace_malformed_metadata_fixture target output_name mode)
    add_library(${target} SHARED
        WorkspaceModuleMalformedMetadataFixture.cpp
    )
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        SAILOR_WORKSPACE_MODULE
        SAILOR_WORKSPACE_MODULE_EXPORTS
        SAILOR_TEST_METADATA_MODE=${mode}
    )
    target_link_libraries(${target} PRIVATE Sailor::Runtime)
    target_include_directories(${target} PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
endfunction()

add_workspace_malformed_metadata_fixture(
    WorkspaceModulePropertyMismatchFixture PropertyMismatchFixture 1)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleDuplicateCdoFixture DuplicateCdoFixture 2)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleUnknownDefaultFixture UnknownDefaultFixture 3)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidDefaultTypeFixture InvalidDefaultTypeFixture 4)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingDefaultFixture MissingDefaultFixture 5)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidEnumDefaultFixture InvalidEnumDefaultFixture 6)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidStructuredDefaultFixture InvalidStructuredDefaultFixture 7)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingCdoFixture MissingCdoFixture 8)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleOversizedStructuredDefaultFixture OversizedStructuredDefaultFixture 9)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleShadowedPropertyFixture ShadowedPropertyFixture 10)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingEmptyPropertySchemaFixture MissingEmptyPropertySchemaFixture 11)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleAliasExpansionFixture AliasExpansionFixture 12)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleInvalidReadOnlyPropertiesFixture InvalidReadOnlyPropertiesFixture 13)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleMissingEnumDefinitionFixture MissingEnumDefinitionFixture 14)
add_workspace_malformed_metadata_fixture(
    WorkspaceModuleRangeMismatchFixture RangeMismatchFixture 15)

add_executable(WorkspaceModuleContractTests
    WorkspaceModuleContractTests.cpp
    WorkspaceModuleFixture.cpp
)

add_executable(WorkspaceModuleRuntimeTests
    WorkspaceModuleRuntimeTests.cpp
)

add_executable(WorkspaceContextTests
    WorkspaceContextTests.cpp
)

add_executable(WorkspaceCacheContractTests
    WorkspaceCacheContractTests.cpp
)

add_executable(AssetCacheContractTests
    AssetCacheContractTests.cpp
)

add_executable(ShaderCacheArtifactTests
    ShaderCacheArtifactTests.cpp
)

add_executable(AssetMountDiscoveryTests
    AssetMountDiscoveryTests.cpp
    ${SAILOR_RUNTIME_DIR}/AssetRegistry/AssetMountDiscovery.cpp
)

if(WIN32)
    add_executable(RemoteViewportWindowsTransportTests
        RemoteViewportWindowsTransportTests.cpp
    )
endif()

if(APPLE)
    add_executable(MacWindowThreadingTests
        MacWindowThreadingTests.mm
    )

    add_executable(RemoteViewportMacTransportTests
        RemoteViewportMacTransportTests.cpp
        ${SAILOR_RUNTIME_DIR}/Submodules/EditorRemote/RemoteViewportMacNativeBridge.mm
    )

    add_executable(RemoteViewportMacNativeBridgeTests
        RemoteViewportMacNativeBridgeTests.mm
        ${SAILOR_RUNTIME_DIR}/Submodules/EditorRemote/RemoteViewportMacNativeBridge.mm
    )
endif()

target_compile_features(RemoteViewportFoundationTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportContractTests PRIVATE cxx_std_20)
target_compile_features(RemoteViewportRuntimeTests PRIVATE cxx_std_20)
target_compile_features(EditorEngineProtocolTests PRIVATE cxx_std_20)
target_compile_features(EditorEngineWebSocketServerTests PRIVATE cxx_std_20)
target_compile_features(EditorViewportSessionTests PRIVATE cxx_std_20)
target_compile_features(SmartPointerTests PRIVATE cxx_std_20)
target_compile_features(InstanceIdContractTests PRIVATE cxx_std_20)
target_compile_features(EcsLifecycleContractTests PRIVATE cxx_std_20)
target_compile_features(BoundsContractTests PRIVATE cxx_std_20)
target_compile_features(GpuCullingContractTests PRIVATE cxx_std_20)
target_compile_definitions(GpuCullingContractTests PRIVATE
    SAILOR_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_compile_features(RHISceneVirtualizationTests PRIVATE cxx_std_20)
target_compile_features(ModelImporterContractTests PRIVATE cxx_std_20)
target_compile_features(AnimationSamplingContractTests PRIVATE cxx_std_20)
target_compile_features(VulkanDescriptorCacheTests PRIVATE cxx_std_20)
target_compile_features(EditorViewportControllerContractTests PRIVATE cxx_std_20)
target_compile_features(SkyComponentContractTests PRIVATE cxx_std_20)
target_compile_features(PhysicsRuntimeTests PRIVATE cxx_std_20)
target_compile_features(AudioContractTests PRIVATE cxx_std_20)
target_compile_features(GraphicsSettingsTests PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleFixture PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleIncompatibleFixture PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleMissingEntryFixture PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleContractTests PRIVATE cxx_std_20)
target_compile_features(WorkspaceModuleRuntimeTests PRIVATE cxx_std_20)
target_compile_features(WorkspaceContextTests PRIVATE cxx_std_20)
target_compile_features(WorkspaceCacheContractTests PRIVATE cxx_std_20)
target_compile_features(AssetCacheContractTests PRIVATE cxx_std_20)
target_compile_features(ShaderCacheArtifactTests PRIVATE cxx_std_20)
target_compile_definitions(ShaderCacheArtifactTests PRIVATE
    SAILOR_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_compile_features(AssetMountDiscoveryTests PRIVATE cxx_std_20)
target_compile_definitions(WorkspaceModuleFixture PRIVATE
    SAILOR_WORKSPACE_MODULE
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_compile_definitions(WorkspaceModuleContractTests PRIVATE
    SAILOR_WORKSPACE_MODULE
)
target_compile_definitions(WorkspaceModuleRuntimeTests PRIVATE
    SAILOR_WORKSPACE_MODULE
)
target_compile_definitions(WorkspaceModuleIncompatibleFixture PRIVATE
    SAILOR_WORKSPACE_NO_ENTRY_POINT_DECLARATIONS
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_compile_definitions(WorkspaceModuleMissingEntryFixture PRIVATE
    SAILOR_WORKSPACE_NO_ENTRY_POINT_DECLARATIONS
    SAILOR_WORKSPACE_MODULE_EXPORTS
)
target_link_libraries(WorkspaceModuleFixture PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceModuleIncompatibleFixture PRIVATE Sailor::Runtime)
target_link_libraries(RemoteViewportRuntimeTests PRIVATE Sailor::Runtime)
target_link_libraries(EditorEngineProtocolTests PRIVATE Sailor::Runtime)
target_link_libraries(EditorEngineWebSocketServerTests PRIVATE
    Sailor::Runtime
    ixwebsocket::ixwebsocket
)
target_link_libraries(EditorViewportSessionTests PRIVATE Sailor::Runtime)
target_link_libraries(SmartPointerTests PRIVATE Sailor::Runtime)
target_link_libraries(InstanceIdContractTests PRIVATE Sailor::Runtime)
target_link_libraries(EcsLifecycleContractTests PRIVATE Sailor::Runtime)
target_link_libraries(BoundsContractTests PRIVATE Sailor::Runtime)
target_link_libraries(GpuCullingContractTests PRIVATE Sailor::Runtime)
target_link_libraries(RHISceneVirtualizationTests PRIVATE Sailor::Runtime)
target_link_libraries(ModelImporterContractTests PRIVATE Sailor::Runtime)
target_link_libraries(AnimationSamplingContractTests PRIVATE Sailor::Runtime)
target_link_libraries(VulkanDescriptorCacheTests PRIVATE Sailor::Runtime)
target_link_libraries(EditorViewportControllerContractTests PRIVATE Sailor::Runtime)
target_link_libraries(SkyComponentContractTests PRIVATE Sailor::Runtime)
target_link_libraries(PhysicsRuntimeTests PRIVATE Sailor::Runtime)
target_link_libraries(AudioContractTests PRIVATE Sailor::Runtime)
target_link_libraries(GraphicsSettingsTests PRIVATE Sailor::Runtime)
target_link_libraries(AssetMountDiscoveryTests PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceModuleContractTests PRIVATE
    Sailor::Runtime
)
target_link_libraries(WorkspaceModuleRuntimeTests PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceContextTests PRIVATE Sailor::Runtime)
target_link_libraries(WorkspaceCacheContractTests PRIVATE Sailor::Runtime)
target_link_libraries(AssetCacheContractTests PRIVATE Sailor::Runtime)
target_link_libraries(ShaderCacheArtifactTests PRIVATE Sailor::Runtime)
add_dependencies(WorkspaceModuleRuntimeTests
    WorkspaceModuleFixture
    WorkspaceModuleIncompatibleFixture
    WorkspaceModuleMissingEntryFixture
    WorkspaceModulePropertyMismatchFixture
    WorkspaceModuleDuplicateCdoFixture
    WorkspaceModuleUnknownDefaultFixture
    WorkspaceModuleInvalidDefaultTypeFixture
    WorkspaceModuleMissingDefaultFixture
    WorkspaceModuleInvalidEnumDefaultFixture
    WorkspaceModuleInvalidStructuredDefaultFixture
    WorkspaceModuleMissingCdoFixture
    WorkspaceModuleOversizedStructuredDefaultFixture
    WorkspaceModuleShadowedPropertyFixture
    WorkspaceModuleMissingEmptyPropertySchemaFixture
    WorkspaceModuleAliasExpansionFixture
    WorkspaceModuleInvalidReadOnlyPropertiesFixture
    WorkspaceModuleMissingEnumDefinitionFixture
    WorkspaceModuleRangeMismatchFixture
)
set_target_properties(WorkspaceModuleFixture PROPERTIES OUTPUT_NAME WorkspaceFixture)
set_target_properties(WorkspaceModuleIncompatibleFixture PROPERTIES OUTPUT_NAME IncompatibleFixture)
set_target_properties(WorkspaceModuleMissingEntryFixture PROPERTIES OUTPUT_NAME MissingEntryFixture)
if(WIN32)
    sailor_stage_workspace_test_runtime(WorkspaceModuleContractTests)
    sailor_stage_workspace_test_runtime(WorkspaceModuleRuntimeTests)
    sailor_stage_workspace_test_runtime(WorkspaceContextTests)
    sailor_stage_workspace_test_runtime(WorkspaceCacheContractTests)
    sailor_stage_workspace_test_runtime(AssetCacheContractTests)
    sailor_stage_workspace_test_runtime(ShaderCacheArtifactTests)
    sailor_stage_workspace_test_runtime(EcsLifecycleContractTests)
    sailor_stage_workspace_test_runtime(BoundsContractTests)
    sailor_stage_workspace_test_runtime(GpuCullingContractTests)
    sailor_stage_workspace_test_runtime(ModelImporterContractTests)
    sailor_stage_workspace_test_runtime(VulkanDescriptorCacheTests)
    sailor_stage_workspace_test_runtime(EditorViewportControllerContractTests)
    sailor_stage_workspace_test_runtime(SkyComponentContractTests)
    sailor_stage_workspace_test_runtime(PhysicsRuntimeTests)
    sailor_stage_workspace_test_runtime(AudioContractTests)
    sailor_stage_workspace_test_runtime(GraphicsSettingsTests)
    sailor_stage_workspace_test_runtime(RemoteViewportRuntimeTests)
    sailor_stage_workspace_test_runtime(EditorEngineProtocolTests)
    sailor_stage_workspace_test_runtime(EditorEngineWebSocketServerTests)
endif()
if(WIN32)
    target_compile_features(RemoteViewportWindowsTransportTests PRIVATE cxx_std_20)
    target_link_libraries(RemoteViewportWindowsTransportTests PRIVATE Sailor::Runtime)
endif()
if(APPLE)
    target_compile_features(MacWindowThreadingTests PRIVATE cxx_std_20)
    target_compile_features(RemoteViewportMacTransportTests PRIVATE cxx_std_20)
    target_compile_features(RemoteViewportMacNativeBridgeTests PRIVATE cxx_std_20)
    target_link_libraries(MacWindowThreadingTests PRIVATE Sailor::Runtime)
    target_link_libraries(RemoteViewportMacTransportTests PRIVATE Sailor::Runtime)
endif()
target_include_directories(RemoteViewportFoundationTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RemoteViewportRuntimeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(EditorEngineProtocolTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
    ${PROJECT_SOURCE_DIR}/Lib
)
target_include_directories(EditorEngineWebSocketServerTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
    ${PROJECT_SOURCE_DIR}/Lib
)
target_include_directories(EditorViewportSessionTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(SmartPointerTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(InstanceIdContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(EcsLifecycleContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(BoundsContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(GpuCullingContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(RHISceneVirtualizationTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(ModelImporterContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(AnimationSamplingContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(VulkanDescriptorCacheTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(EditorViewportControllerContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(SkyComponentContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(PhysicsRuntimeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(AudioContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(GraphicsSettingsTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleFixture PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleIncompatibleFixture PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleMissingEntryFixture PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceModuleRuntimeTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceContextTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(WorkspaceCacheContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(AssetCacheContractTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(ShaderCacheArtifactTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
target_include_directories(AssetMountDiscoveryTests PRIVATE
    ${SAILOR_RUNTIME_DIR}
)
if(WIN32)
    target_include_directories(RemoteViewportWindowsTransportTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
endif()

if(APPLE)
    target_include_directories(MacWindowThreadingTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    target_include_directories(RemoteViewportMacTransportTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    target_include_directories(RemoteViewportMacNativeBridgeTests PRIVATE
        ${SAILOR_RUNTIME_DIR}
    )
    target_link_libraries(MacWindowThreadingTests PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore")
    target_link_libraries(RemoteViewportMacTransportTests PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOSurface")
    target_link_libraries(RemoteViewportMacNativeBridgeTests PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOSurface")
endif()

add_test(
    NAME RemoteViewportFoundationTests
    COMMAND RemoteViewportFoundationTests
)

add_test(
    NAME RemoteViewportContractTests
    COMMAND RemoteViewportContractTests
)

add_test(
    NAME RemoteViewportRuntimeTests
    COMMAND RemoteViewportRuntimeTests
)

add_test(
    NAME EditorEngineProtocolTests
    COMMAND EditorEngineProtocolTests
)

add_test(
    NAME EditorEngineWebSocketServerTests
    COMMAND EditorEngineWebSocketServerTests
)

add_test(
    NAME EditorEngineWebSocketLateStopTests
    COMMAND EditorEngineWebSocketServerTests
        --late-stop-after-static-teardown
)

add_test(
    NAME EditorViewportSessionTests
    COMMAND EditorViewportSessionTests
)

add_test(
    NAME SmartPointerTests
    COMMAND SmartPointerTests
)

add_test(
    NAME InstanceIdContractTests
    COMMAND InstanceIdContractTests
)

add_test(
    NAME EcsLifecycleContractTests
    COMMAND EcsLifecycleContractTests
)

add_test(
    NAME BoundsContractTests
    COMMAND BoundsContractTests
)

add_test(
    NAME GpuCullingContractTests
    COMMAND GpuCullingContractTests
)

add_test(
    NAME RHISceneVirtualizationTests
    COMMAND RHISceneVirtualizationTests
)

add_test(
    NAME ModelImporterContractTests
    COMMAND ModelImporterContractTests
)

add_test(
    NAME AnimationSamplingContractTests
    COMMAND AnimationSamplingContractTests
)

add_test(
    NAME VulkanDescriptorCacheTests
    COMMAND VulkanDescriptorCacheTests
)

add_test(
    NAME EditorViewportControllerContractTests
    COMMAND EditorViewportControllerContractTests
)

add_test(
    NAME SkyComponentContractTests
    COMMAND SkyComponentContractTests
)

add_test(
    NAME PhysicsRuntimeTests
    COMMAND PhysicsRuntimeTests
)

add_test(
    NAME AudioContractTests
    COMMAND AudioContractTests
)

add_test(
    NAME GraphicsSettingsTests
    COMMAND GraphicsSettingsTests
)

add_test(
    NAME WorkspaceModuleContractTests
    COMMAND WorkspaceModuleContractTests
)

add_test(
    NAME WorkspaceContextTests
    COMMAND WorkspaceContextTests
)

add_test(
    NAME WorkspaceCacheContractTests
    COMMAND WorkspaceCacheContractTests
)

add_test(
    NAME AssetCacheContractTests
    COMMAND AssetCacheContractTests
)

add_test(
    NAME ShaderCacheArtifactTests
    COMMAND ShaderCacheArtifactTests
)

add_test(
    NAME AssetMountDiscoveryTests
    COMMAND AssetMountDiscoveryTests "${PROJECT_SOURCE_DIR}/Content"
)

add_test(
    NAME WorkspaceModuleRuntimeTests
    COMMAND WorkspaceModuleRuntimeTests
        $<TARGET_FILE:WorkspaceModuleFixture>
        $<TARGET_FILE:WorkspaceModuleIncompatibleFixture>
        $<TARGET_FILE:WorkspaceModuleMissingEntryFixture>
        $<TARGET_FILE:WorkspaceModulePropertyMismatchFixture>
        $<TARGET_FILE:WorkspaceModuleDuplicateCdoFixture>
        $<TARGET_FILE:WorkspaceModuleUnknownDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidDefaultTypeFixture>
        $<TARGET_FILE:WorkspaceModuleMissingDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidEnumDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidStructuredDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleMissingCdoFixture>
        $<TARGET_FILE:WorkspaceModuleOversizedStructuredDefaultFixture>
        $<TARGET_FILE:WorkspaceModuleShadowedPropertyFixture>
        $<TARGET_FILE:WorkspaceModuleMissingEmptyPropertySchemaFixture>
        $<TARGET_FILE:WorkspaceModuleAliasExpansionFixture>
        $<TARGET_FILE:WorkspaceModuleInvalidReadOnlyPropertiesFixture>
        $<TARGET_FILE:WorkspaceModuleMissingEnumDefinitionFixture>
        $<TARGET_FILE:WorkspaceModuleRangeMismatchFixture>
        $<CONFIG>
)
set_tests_properties(
    WorkspaceContextTests
    WorkspaceCacheContractTests
    AssetCacheContractTests
    ShaderCacheArtifactTests
    AssetMountDiscoveryTests
    WorkspaceModuleContractTests
    WorkspaceModuleRuntimeTests
    SmartPointerTests
    InstanceIdContractTests
    EcsLifecycleContractTests
    BoundsContractTests
    GpuCullingContractTests
    RHISceneVirtualizationTests
    ModelImporterContractTests
    AnimationSamplingContractTests
    VulkanDescriptorCacheTests
    EditorViewportControllerContractTests
    SkyComponentContractTests
    PhysicsRuntimeTests
    AudioContractTests
    GraphicsSettingsTests
    EditorEngineProtocolTests
    EditorEngineWebSocketServerTests
    EditorEngineWebSocketLateStopTests
    PROPERTIES TIMEOUT 30
)

if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.22" AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    set_tests_properties(
        WorkspaceContextTests
        WorkspaceCacheContractTests
        AssetCacheContractTests
        ShaderCacheArtifactTests
        GraphicsSettingsTests
        WorkspaceModuleContractTests
        WorkspaceModuleRuntimeTests
        PROPERTIES ENVIRONMENT_MODIFICATION
            "Path=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin,bin>"
    )
endif()

if(WIN32)
    add_test(
        NAME RemoteViewportWindowsTransportTests
        COMMAND RemoteViewportWindowsTransportTests
    )
endif()

if(APPLE)
    add_test(
        NAME MacWindowThreadingTests
        COMMAND MacWindowThreadingTests
    )
    set_tests_properties(MacWindowThreadingTests PROPERTIES TIMEOUT 15)

    add_test(
        NAME RemoteViewportMacTransportTests
        COMMAND RemoteViewportMacTransportTests
    )

    add_test(
        NAME RemoteViewportMacNativeBridgeTests
        COMMAND RemoteViewportMacNativeBridgeTests
    )
endif()
