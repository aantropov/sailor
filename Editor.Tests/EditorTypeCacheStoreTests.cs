using SailorEditor.Services;

namespace Editor.Tests;

public sealed class EditorTypeCacheStoreTests : IDisposable
{
    static readonly EditorTypeCacheIdentity WorkspaceA = new(
        "workspace-a",
        "0.1",
        "build-debug-msvc",
        "editor-types-v1");
    static readonly string Catalog = """
        engineTypes:
          - typename: Sailor::TransformComponent
        cdo: []
        enums: {}
        assetTypes: []
        """;

    readonly string _directory = Path.Combine(
        Path.GetTempPath(),
        $"SailorEditorTypeCacheTests-{Guid.NewGuid():N}");

    string CachePath => Path.Combine(_directory, "EditorTypes.yaml");

    [Fact]
    public void SaveAndLoad_RoundTripsStrictEnvelopeAndOpaqueCatalog()
    {
        var store = new EditorTypeCacheStore();

        var write = store.Save(CachePath, WorkspaceA, Catalog);
        var load = store.Load(CachePath, WorkspaceA);
        var envelope = File.ReadAllText(CachePath);

        Assert.True(write.Succeeded, write.Diagnostic);
        Assert.Equal(EditorTypeCacheStatus.Loaded, load.Status);
        Assert.True(load.Succeeded);
        Assert.Equal(Catalog, load.Payload);
        Assert.Contains("cacheVersion: 1", envelope, StringComparison.Ordinal);
        Assert.Contains("cacheKind: editor-types", envelope, StringComparison.Ordinal);
        Assert.Contains("workspaceId: workspace-a", envelope, StringComparison.Ordinal);
        Assert.Contains("payloadVersion: 1", envelope, StringComparison.Ordinal);
    }

    [Fact]
    public void ParseNativeIdentity_RequiresTheExactNativeBridgeContract()
    {
        var identity = EditorTypeCacheStore.ParseNativeIdentity("""
            workspaceIdentity: рабочая-область-а
            engineVersion: 0.1
            buildIdentity: sailor-abi-debug
            producerIdentity: editor-types-v1
            """);

        Assert.Equal("рабочая-область-а", identity.WorkspaceIdentity);
        Assert.Equal("0.1", identity.EngineVersion);
        Assert.Equal("sailor-abi-debug", identity.BuildIdentity);
        Assert.Equal("editor-types-v1", identity.ProducerIdentity);
    }

    [Theory]
    [InlineData("workspaceIdentity: workspace-a\nengineVersion: 0.1\nbuildIdentity: build\n")]
    [InlineData("workspaceIdentity: workspace-a\nengineVersion: 0.1\nbuildIdentity: build\nproducerIdentity: editor-types-v1\nextra: value\n")]
    public void ParseNativeIdentity_RejectsIncompleteOrUnknownFields(string yaml)
    {
        Assert.Throws<InvalidDataException>(() => EditorTypeCacheStore.ParseNativeIdentity(yaml));
    }

    [Fact]
    public void Load_RejectsWorkspaceACacheForWorkspaceB()
    {
        var store = new EditorTypeCacheStore();
        Assert.True(store.Save(CachePath, WorkspaceA, Catalog).Succeeded);
        var workspaceB = WorkspaceA with { WorkspaceIdentity = "workspace-b" };

        var result = store.Load(CachePath, workspaceB);

        Assert.Equal(EditorTypeCacheStatus.StaleIdentity, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("workspaceId", result.Diagnostic, StringComparison.Ordinal);
        Assert.Contains("workspace-a", result.Diagnostic, StringComparison.Ordinal);
        Assert.Contains("workspace-b", result.Diagnostic, StringComparison.Ordinal);
    }

    [Fact]
    public void Load_AtoBtoA_NeverPublishesTheOtherWorkspaceCatalog()
    {
        var store = new EditorTypeCacheStore();
        var workspaceAPath = Path.Combine(_directory, "A", "EditorTypes.yaml");
        var workspaceBPath = Path.Combine(_directory, "B", "EditorTypes.yaml");
        var workspaceB = WorkspaceA with { WorkspaceIdentity = "workspace-b" };
        var workspaceBCatalog = Catalog.Replace(
            "Sailor::TransformComponent",
            "Game::WorkspaceBComponent",
            StringComparison.Ordinal);

        Assert.True(store.Save(workspaceAPath, WorkspaceA, Catalog).Succeeded);
        Directory.CreateDirectory(Path.GetDirectoryName(workspaceBPath)!);
        File.Copy(workspaceAPath, workspaceBPath);

        var staleInB = store.Load(workspaceBPath, workspaceB);
        Assert.Equal(EditorTypeCacheStatus.StaleIdentity, staleInB.Status);
        Assert.Null(staleInB.Payload);
        Assert.True(store.Invalidate(workspaceBPath).Succeeded);
        Assert.True(store.Save(workspaceBPath, workspaceB, workspaceBCatalog).Succeeded);
        Assert.Equal(workspaceBCatalog, store.Load(workspaceBPath, workspaceB).Payload);

        var restoredA = store.Load(workspaceAPath, WorkspaceA);
        Assert.Equal(EditorTypeCacheStatus.Loaded, restoredA.Status);
        Assert.Equal(Catalog, restoredA.Payload);
        Assert.DoesNotContain("WorkspaceBComponent", restoredA.Payload, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("EngineVersion", "0.2", "engineVersion")]
    [InlineData("BuildIdentity", "build-release-clang", "buildIdentity")]
    [InlineData("ProducerIdentity", "editor-types-v2", "producerIdentity")]
    public void Load_RejectsEachStaleProducerIdentity(
        string property,
        string replacement,
        string diagnosticField)
    {
        var store = new EditorTypeCacheStore();
        Assert.True(store.Save(CachePath, WorkspaceA, Catalog).Succeeded);
        var expected = property switch
        {
            nameof(EditorTypeCacheIdentity.EngineVersion) => WorkspaceA with { EngineVersion = replacement },
            nameof(EditorTypeCacheIdentity.BuildIdentity) => WorkspaceA with { BuildIdentity = replacement },
            nameof(EditorTypeCacheIdentity.ProducerIdentity) => WorkspaceA with { ProducerIdentity = replacement },
            _ => throw new ArgumentOutOfRangeException(nameof(property))
        };

        var result = store.Load(CachePath, expected);

        Assert.Equal(EditorTypeCacheStatus.StaleIdentity, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains(diagnosticField, result.Diagnostic, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("engineTypes: []\n")]
    [InlineData("cacheVersion: 0\n")]
    [InlineData("cacheVersion: 2\n")]
    [InlineData("cacheVersion: 1\ncacheKind: editor-types\nworkspaceId: workspace-a\nengineVersion: 0.1\nbuildIdentity: build-debug-msvc\nproducerIdentity: editor-types-v1\npayload: '{}'")]
    [InlineData("cacheVersion: 1\npayloadVersion: 0\n")]
    [InlineData("cacheVersion: 1\npayloadVersion: 2\n")]
    public void Load_RejectsRawMissingOldAndFutureVersionsAsUnsupported(string yaml)
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(CachePath, yaml);

        var result = new EditorTypeCacheStore().Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.UnsupportedVersion, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("unsupported", result.Diagnostic, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("cacheVersion: [")]
    [InlineData("cacheVersion: one\npayloadVersion: 1\n")]
    [InlineData("cacheVersion: 1\npayloadVersion: one\n")]
    public void Load_ReportsMalformedEnvelopeAsCorrupt(string yaml)
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(CachePath, yaml);

        var result = new EditorTypeCacheStore().Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.Corrupt, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("corrupt", result.Diagnostic, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Load_ReportsMalformedOpaquePayloadAsCorrupt()
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllText(CachePath, """
            cacheVersion: 1
            cacheKind: editor-types
            workspaceId: workspace-a
            engineVersion: 0.1
            buildIdentity: build-debug-msvc
            producerIdentity: editor-types-v1
            payloadVersion: 1
            payload: |
              engineTypes: [
            """);

        var result = new EditorTypeCacheStore().Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.Corrupt, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("Payload", result.Diagnostic, StringComparison.Ordinal);
    }

    [Fact]
    public void Load_ReportsInvalidUtf8AsCorruptRatherThanIoFailure()
    {
        Directory.CreateDirectory(_directory);
        File.WriteAllBytes(CachePath, [0xc3, 0x28]);

        var result = new EditorTypeCacheStore().Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.Corrupt, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("UTF-8", result.Diagnostic, StringComparison.Ordinal);
    }

    [Fact]
    public void Load_ReturnsMissingWithoutPayload()
    {
        var result = new EditorTypeCacheStore().Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.Missing, result.Status);
        Assert.Null(result.Payload);
    }

    [Fact]
    public void Load_MapsFilesystemFailureToIoFailure()
    {
        var store = new EditorTypeCacheStore(new ThrowingReadFileOperations());

        var result = store.Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.IoFailure, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("injected read failure", result.Diagnostic, StringComparison.Ordinal);
    }

    [Fact]
    public void Load_DirectoryAtCachePathIsIoFailureRatherThanMissing()
    {
        Directory.CreateDirectory(CachePath);

        var result = new EditorTypeCacheStore().Load(CachePath, WorkspaceA);

        Assert.Equal(EditorTypeCacheStatus.IoFailure, result.Status);
        Assert.Null(result.Payload);
        Assert.Contains("Cannot read", result.Diagnostic, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData((int)EditorTypeCacheStatus.StaleIdentity, true)]
    [InlineData((int)EditorTypeCacheStatus.Corrupt, true)]
    [InlineData((int)EditorTypeCacheStatus.UnsupportedVersion, true)]
    [InlineData((int)EditorTypeCacheStatus.Missing, false)]
    [InlineData((int)EditorTypeCacheStatus.Loaded, false)]
    [InlineData((int)EditorTypeCacheStatus.IoFailure, false)]
    public void InvalidationPolicy_PreservesFilesAfterIoFailure(
        int statusValue,
        bool expected)
    {
        Assert.Equal(
            expected,
            EditorTypeCacheStore.ShouldInvalidate((EditorTypeCacheStatus)statusValue));
    }

    [Theory]
    [InlineData((int)EditorTypeCacheStatus.Missing, true)]
    [InlineData((int)EditorTypeCacheStatus.Loaded, true)]
    [InlineData((int)EditorTypeCacheStatus.StaleIdentity, true)]
    [InlineData((int)EditorTypeCacheStatus.Corrupt, true)]
    [InlineData((int)EditorTypeCacheStatus.UnsupportedVersion, true)]
    [InlineData((int)EditorTypeCacheStatus.IoFailure, false)]
    public void PersistencePolicy_DoesNotReplaceStorageAfterIoFailure(
        int statusValue,
        bool expected)
    {
        Assert.Equal(
            expected,
            EditorTypeCacheStore.ShouldPersistLiveCatalog((EditorTypeCacheStatus)statusValue));
    }

    [Fact]
    public void Save_ReplaceFailurePreservesPreviousTargetAndCleansTemporaryFile()
    {
        var physicalStore = new EditorTypeCacheStore();
        Assert.True(physicalStore.Save(CachePath, WorkspaceA, Catalog).Succeeded);
        var previousContents = File.ReadAllText(CachePath);
        var replacementCatalog = "engineTypes: []\ncdo: []\nenums: {}\nassetTypes: []\n";
        var failingStore = new EditorTypeCacheStore(new FailingReplaceFileOperations());

        var result = failingStore.Save(CachePath, WorkspaceA, replacementCatalog);

        Assert.False(result.Succeeded);
        Assert.Contains("injected replace failure", result.Diagnostic, StringComparison.Ordinal);
        Assert.Equal(previousContents, File.ReadAllText(CachePath));
        Assert.Empty(Directory.EnumerateFiles(_directory, "*.tmp"));
    }

    [Fact]
    public void Save_CorruptPayloadDoesNotTouchPreviousTarget()
    {
        var store = new EditorTypeCacheStore();
        Assert.True(store.Save(CachePath, WorkspaceA, Catalog).Succeeded);
        var previousContents = File.ReadAllText(CachePath);

        var result = store.Save(CachePath, WorkspaceA, "engineTypes: [");

        Assert.False(result.Succeeded);
        Assert.Contains("payload is corrupt", result.Diagnostic, StringComparison.Ordinal);
        Assert.Equal(previousContents, File.ReadAllText(CachePath));
        Assert.Empty(Directory.EnumerateFiles(_directory, "*.tmp"));
    }

    [Fact]
    public void Invalidate_RemovesOnlyTargetAndOwnedOrphanTemporaryFiles()
    {
        var store = new EditorTypeCacheStore();
        Assert.True(store.Save(CachePath, WorkspaceA, Catalog).Succeeded);
        var neighbor = Path.Combine(_directory, "ShaderCache.yaml");
        var lookalike = Path.Combine(_directory, ".EditorTypes.yaml.user.tmp");
        var ownedTemporary = Path.Combine(
            _directory,
            $".EditorTypes.yaml.{Guid.NewGuid():N}.tmp");
        File.WriteAllText(neighbor, "keep");
        File.WriteAllText(lookalike, "keep");
        File.WriteAllText(ownedTemporary, "discard");

        var result = store.Invalidate(CachePath);
        var missingResult = store.Invalidate(CachePath);

        Assert.True(result.Succeeded, result.Diagnostic);
        Assert.True(missingResult.Succeeded, missingResult.Diagnostic);
        Assert.False(File.Exists(CachePath));
        Assert.False(File.Exists(ownedTemporary));
        Assert.Equal("keep", File.ReadAllText(neighbor));
        Assert.Equal("keep", File.ReadAllText(lookalike));
    }

    [Fact]
    public void Invalidate_ReportsIoFailureWithoutDeletingNeighboringFiles()
    {
        Directory.CreateDirectory(_directory);
        var neighbor = Path.Combine(_directory, "ShaderCache.yaml");
        File.WriteAllText(CachePath, "old cache");
        File.WriteAllText(neighbor, "keep");
        var store = new EditorTypeCacheStore(new FailingDeleteFileOperations());

        var result = store.Invalidate(CachePath);

        Assert.False(result.Succeeded);
        Assert.Contains("injected delete failure", result.Diagnostic, StringComparison.Ordinal);
        Assert.True(File.Exists(CachePath));
        Assert.Equal("keep", File.ReadAllText(neighbor));
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
            Directory.Delete(_directory, recursive: true);
    }

    sealed class FailingReplaceFileOperations : IEditorTypeCacheFileOperations
    {
        readonly PhysicalEditorTypeCacheFileOperations _physical = new();

        public string ReadAllText(string path) => _physical.ReadAllText(path);
        public void CreateDirectory(string path) => _physical.CreateDirectory(path);
        public void WriteAllTextDurably(string path, string contents)
            => _physical.WriteAllTextDurably(path, contents);
        public void Replace(string temporaryPath, string targetPath)
            => throw new IOException("injected replace failure");
        public void DeleteIfExists(string path) => _physical.DeleteIfExists(path);
        public IEnumerable<string> EnumerateFiles(string directory) => _physical.EnumerateFiles(directory);
    }

    sealed class ThrowingReadFileOperations : IEditorTypeCacheFileOperations
    {
        public string ReadAllText(string path) => throw new IOException("injected read failure");
        public void CreateDirectory(string path) => throw new NotSupportedException();
        public void WriteAllTextDurably(string path, string contents) => throw new NotSupportedException();
        public void Replace(string temporaryPath, string targetPath) => throw new NotSupportedException();
        public void DeleteIfExists(string path) => throw new NotSupportedException();
        public IEnumerable<string> EnumerateFiles(string directory) => throw new NotSupportedException();
    }

    sealed class FailingDeleteFileOperations : IEditorTypeCacheFileOperations
    {
        readonly PhysicalEditorTypeCacheFileOperations _physical = new();

        public string ReadAllText(string path) => _physical.ReadAllText(path);
        public void CreateDirectory(string path) => _physical.CreateDirectory(path);
        public void WriteAllTextDurably(string path, string contents)
            => _physical.WriteAllTextDurably(path, contents);
        public void Replace(string temporaryPath, string targetPath)
            => _physical.Replace(temporaryPath, targetPath);
        public void DeleteIfExists(string path) => throw new IOException("injected delete failure");
        public IEnumerable<string> EnumerateFiles(string directory) => _physical.EnumerateFiles(directory);
    }
}
