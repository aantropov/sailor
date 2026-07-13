using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using SailorEditor.Services;
using SailorEditor.Workspace;
using SailorEngine;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceTwoWorkspaceCloseoutIntegrationTests
{
    const int ScenarioSchemaVersion = 1;
    const string ScenarioRootEnvironmentVariable = "SAILOR_WORKSPACE_E2E_ROOT";
    const string ScenarioDescriptorFileName = "TwoWorkspaceScenario.json";
    const string AssetRelativePath = "Shared/Identity.asset";
    const string WorkspaceAModuleName = "CloseoutAlphaLogic";
    const string WorkspaceBModuleName = "CloseoutBetaLogic";
    const string WorkspaceAAssetFileId = "{E2E00000-0000-0000-0000-00000000000A}";
    const string WorkspaceBAssetFileId = "{E2E00000-0000-0000-0000-00000000000B}";

    static readonly string[] ProtectedWorkspaceAFiles =
    [
        ".gitignore",
        "Generated/CMakeLists.txt",
        "Source/Components/SampleComponent.h",
        "Source/Components/SampleComponent.cpp",
        "Source/WorkspaceTypes.h",
        "Source/WorkspaceModule.cpp",
        WorkspaceGeneratedProjectStateService.RelativePath,
        $"Content/{AssetRelativePath}"
    ];

    [Fact]
    public void ScenarioDescriptor_RoundTripsPhaseBoundaryData()
    {
        var workspaceA = new WorkspaceScenarioEntry(
            "/scenario/A",
            "/scenario/A/workspace.sailor",
            "workspace-a",
            "Saved A",
            WorkspaceAModuleName,
            $"{WorkspaceAModuleName}::SampleComponent",
            11.0f,
            AssetRelativePath,
            WorkspaceAAssetFileId,
            BuildAssetContents(WorkspaceAAssetFileId, 101),
            "/scenario/A/Binaries/Release/CloseoutAlphaLogic.dll");
        var descriptor = new ScenarioDescriptor(
            ScenarioSchemaVersion,
            "/scenario",
            workspaceA,
            workspaceA with
            {
                WorkspaceRoot = "/scenario/B",
                WorkspaceId = "workspace-b",
                ModuleName = WorkspaceBModuleName
            },
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                [ProtectedWorkspaceAFiles[0]] = "sha256"
            });

        var roundTrip = JsonSerializer.Deserialize<ScenarioDescriptor>(
            JsonSerializer.Serialize(descriptor));

        Assert.NotNull(roundTrip);
        Assert.Equal(descriptor.SchemaVersion, roundTrip!.SchemaVersion);
        Assert.Equal(descriptor.WorkspaceA, roundTrip.WorkspaceA);
        Assert.Equal(descriptor.WorkspaceB.WorkspaceId, roundTrip.WorkspaceB.WorkspaceId);
        Assert.Equal("sha256", roundTrip.WorkspaceAProtectedFileHashes[ProtectedWorkspaceAFiles[0]]);
    }

    [Fact]
    [Trait("Category", "CMakeIntegration")]
    public async Task Phase1_CreatesBuildsActivatesSavesAndPersistsScenario()
    {
        if (!WorkspaceCMakeIntegrationHarness.ShouldRun)
            return;

        WorkspaceCMakeIntegrationHarness.EnsureWindows();
        var scenarioRoot = GetScenarioRoot();
        ResetScenarioRoot(scenarioRoot);
        var installPrefix = Path.GetFullPath(
            WorkspaceCMakeIntegrationHarness.GetRequiredEnvironmentVariable(
                "SAILOR_ENGINE_INSTALL_PREFIX"));
        var vcpkgRoot = Path.GetFullPath(
            WorkspaceCMakeIntegrationHarness.GetRequiredEnvironmentVariable("VCPKG_ROOT"));

        var workspaceA = await CreateBuildWorkspaceAsync(
            scenarioRoot,
            "WorkspaceA",
            "workspace-closeout-alpha",
            "Closeout Alpha",
            WorkspaceAModuleName,
            11.0f,
            WorkspaceAAssetFileId,
            101,
            installPrefix);
        var workspaceB = await CreateBuildWorkspaceAsync(
            scenarioRoot,
            "WorkspaceB",
            "workspace-closeout-beta",
            "Closeout Beta",
            WorkspaceBModuleName,
            22.0f,
            WorkspaceBAssetFileId,
            202,
            installPrefix);

        Assert.NotEqual(workspaceA.ModulePath, workspaceB.ModulePath);
        Assert.NotEqual(HashFile(workspaceA.ModulePath), HashFile(workspaceB.ModulePath));
        Assert.NotEqual(workspaceA.ComponentTypeName, workspaceB.ComponentTypeName);

        var lifecycle = CreateLifecycle(scenarioRoot);
        var operations = new HeadlessWorkspaceActivationOperations(
            lifecycle,
            installPrefix,
            vcpkgRoot,
            AssetRelativePath);
        try
        {
            var coordinator = new WorkspaceActivationCoordinator(operations);
            var activatedA = await ActivateAsync(coordinator, lifecycle, workspaceA);
            AssertActive(
                activatedA,
                operations,
                workspaceA,
                workspaceB,
                expectedGeneration: 1,
                EditorTypeCacheStatus.Missing);

            var workspaceACache = activatedA.Candidate!.LaunchContext.EditorTypesCacheFilePath;
            var workspaceBCache = Path.Combine(workspaceB.WorkspaceRoot, "Cache", "EditorTypes.yaml");
            Assert.True(File.Exists(workspaceACache));
            Assert.False(File.Exists(workspaceBCache));
            File.Copy(workspaceACache, workspaceBCache);

            var activatedB = await ActivateAsync(coordinator, lifecycle, workspaceB);
            AssertActive(
                activatedB,
                operations,
                workspaceB,
                workspaceA,
                expectedGeneration: 2,
                EditorTypeCacheStatus.StaleIdentity);

            var reactivatedA = await ActivateAsync(coordinator, lifecycle, workspaceA);
            AssertActive(
                reactivatedA,
                operations,
                workspaceA,
                workspaceB,
                expectedGeneration: 3,
                EditorTypeCacheStatus.Loaded);

            Assert.Equal(
                [
                    "stop", "clear:1", $"commit:{WorkspaceAModuleName}", $"start:{WorkspaceAModuleName}:1",
                    "stop", "clear:2", $"commit:{WorkspaceBModuleName}", $"start:{WorkspaceBModuleName}:2",
                    "stop", "clear:3", $"commit:{WorkspaceAModuleName}", $"start:{WorkspaceAModuleName}:3"
                ],
                operations.Events);
            Assert.Equal(2, operations.UnloadCount);

            var protectedHashesBeforeSave = HashFiles(workspaceA.WorkspaceRoot, ProtectedWorkspaceAFiles);
            var savedName = "Closeout Alpha Saved";
            WorkspaceLifecycleResult? saveResult = null;
            await coordinator.RunSerializedAsync(async cancellationToken =>
            {
                saveResult = await lifecycle.SaveAsync(
                    reactivatedA.Candidate!.Session with
                    {
                        Manifest = reactivatedA.Candidate.Session.Manifest with { Name = savedName }
                    },
                    cancellationToken);
            });

            Assert.NotNull(saveResult);
            Assert.True(saveResult!.Succeeded, saveResult.Error);
            Assert.Equal(savedName, saveResult.Session!.Manifest.Name);
            Assert.Equal(
                WorkspaceGeneratedProjectStateStatus.Current,
                saveResult.Session.GeneratedProjectState.Status);
            AssertHashes(workspaceA.WorkspaceRoot, protectedHashesBeforeSave);
            var protectedHashesAfterSave = HashFiles(workspaceA.WorkspaceRoot, ProtectedWorkspaceAFiles);

            workspaceA = workspaceA with { ExpectedSavedName = savedName };
            var descriptor = new ScenarioDescriptor(
                ScenarioSchemaVersion,
                scenarioRoot,
                workspaceA,
                workspaceB,
                protectedHashesAfterSave);

            operations.Dispose();
            Assert.Equal(3, operations.UnloadCount);
            await PersistScenarioDescriptorLastAsync(scenarioRoot, descriptor);
        }
        finally
        {
            operations.Dispose();
        }
    }

    [Fact]
    [Trait("Category", "CMakeIntegration")]
    public async Task Phase2_FreshProcessReopensValidatesAndCleansScenario()
    {
        if (!WorkspaceCMakeIntegrationHarness.ShouldRun)
            return;

        WorkspaceCMakeIntegrationHarness.EnsureWindows();
        var scenarioRoot = GetScenarioRoot();
        var verificationSucceeded = false;
        try
        {
            var descriptor = await LoadScenarioDescriptorAsync(scenarioRoot);
            ValidateDescriptor(descriptor, scenarioRoot);
            var installPrefix = Path.GetFullPath(
                WorkspaceCMakeIntegrationHarness.GetRequiredEnvironmentVariable(
                    "SAILOR_ENGINE_INSTALL_PREFIX"));
            var vcpkgRoot = Path.GetFullPath(
                WorkspaceCMakeIntegrationHarness.GetRequiredEnvironmentVariable("VCPKG_ROOT"));
            var lifecycle = CreateLifecycle(scenarioRoot);
            var persistedRecents = await lifecycle.LoadRecentAsync();
            Assert.Equal(2, persistedRecents.Count);
            var recentA = Assert.Single(
                persistedRecents,
                entry => string.Equals(
                    entry.WorkspaceId,
                    descriptor.WorkspaceA.WorkspaceId,
                    StringComparison.Ordinal));
            var recentB = Assert.Single(
                persistedRecents,
                entry => string.Equals(
                    entry.WorkspaceId,
                    descriptor.WorkspaceB.WorkspaceId,
                    StringComparison.Ordinal));
            Assert.Equal(descriptor.WorkspaceA.ExpectedSavedName, recentA.Name);
            Assert.Equal(descriptor.WorkspaceA.ManifestPath, recentA.ManifestPath);
            Assert.Equal(descriptor.WorkspaceB.ExpectedSavedName, recentB.Name);
            Assert.Equal(descriptor.WorkspaceB.ManifestPath, recentB.ManifestPath);

            {
                using var operations = new HeadlessWorkspaceActivationOperations(
                    lifecycle,
                    installPrefix,
                    vcpkgRoot,
                    AssetRelativePath);
                var coordinator = new WorkspaceActivationCoordinator(operations);
                var activatedA = await ActivateAsync(coordinator, lifecycle, descriptor.WorkspaceA);
                AssertActive(
                    activatedA,
                    operations,
                    descriptor.WorkspaceA,
                    descriptor.WorkspaceB,
                    expectedGeneration: 1,
                    EditorTypeCacheStatus.Loaded);
                Assert.Equal(
                    descriptor.WorkspaceA.ExpectedSavedName,
                    activatedA.Candidate!.Session.Manifest.Name);
                Assert.Equal(
                    WorkspaceGeneratedProjectStateStatus.Current,
                    activatedA.Candidate.Session.GeneratedProjectState.Status);
                AssertHashes(
                    descriptor.WorkspaceA.WorkspaceRoot,
                    descriptor.WorkspaceAProtectedFileHashes);

                var activatedB = await ActivateAsync(coordinator, lifecycle, descriptor.WorkspaceB);
                AssertActive(
                    activatedB,
                    operations,
                    descriptor.WorkspaceB,
                    descriptor.WorkspaceA,
                    expectedGeneration: 2,
                    EditorTypeCacheStatus.Loaded);
                Assert.Same(activatedB.Candidate!.Session, lifecycle.Current);
                AssertHashes(
                    descriptor.WorkspaceA.WorkspaceRoot,
                    descriptor.WorkspaceAProtectedFileHashes);
                Assert.Equal(
                    [
                        "stop", "clear:1", $"commit:{WorkspaceAModuleName}", $"start:{WorkspaceAModuleName}:1",
                        "stop", "clear:2", $"commit:{WorkspaceBModuleName}", $"start:{WorkspaceBModuleName}:2"
                    ],
                    operations.Events);
            }
            verificationSucceeded = true;
        }
        finally
        {
            if (verificationSucceeded)
                DeleteScenarioRoot(scenarioRoot);
        }
    }

    static async Task<WorkspaceScenarioEntry> CreateBuildWorkspaceAsync(
        string scenarioRoot,
        string directoryName,
        string workspaceId,
        string workspaceName,
        string moduleName,
        float moveSpeed,
        string assetFileId,
        long assetImportTime,
        string installPrefix)
    {
        var workspaceRoot = Path.Combine(scenarioRoot, directoryName);
        var serializer = new WorkspaceManifestSerializer();
        var generatedState = new WorkspaceGeneratedProjectStateService();
        var generator = new WorkspaceProjectGenerator(generatedState);
        var template = new ConfiguredWorkspaceTemplateService(
            serializer,
            generator,
            moduleName);
        var createLifecycle = new WorkspaceLifecycleService(
            serializer,
            template,
            new RecentWorkspaceStore(GetRecentsPath(scenarioRoot)),
            generatedState);
        var created = await createLifecycle.CreateAsync(new WorkspaceCreateRequest(
            workspaceName,
            workspaceRoot,
            installPrefix,
            workspaceId,
            EngineReferenceKind: WorkspaceEngineReferenceKinds.Installed));
        Assert.True(created.Succeeded, created.Error);
        var session = created.Session!;
        Assert.Equal(moduleName, session.Manifest.LogicModuleName);
        Assert.Equal(
            WorkspaceGeneratedProjectStateStatus.Current,
            (await generatedState.AssessAsync(workspaceRoot, session.Manifest)).Status);

        var componentHeaderPath = Path.Combine(
            session.SourceDirectory,
            "Components",
            "SampleComponent.h");
        var componentHeader = await File.ReadAllTextAsync(componentHeaderPath);
        var customizedHeader = componentHeader.Replace(
            "m_moveSpeed = 5.0f",
            $"m_moveSpeed = {moveSpeed.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture)}f",
            StringComparison.Ordinal);
        Assert.NotEqual(componentHeader, customizedHeader);
        await File.WriteAllTextAsync(componentHeaderPath, customizedHeader, new UTF8Encoding(false));

        var assetPath = ResolveRelativePath(session.ContentDirectory, AssetRelativePath);
        Directory.CreateDirectory(Path.GetDirectoryName(assetPath)!);
        var assetContents = BuildAssetContents(assetFileId, assetImportTime);
        await File.WriteAllTextAsync(assetPath, assetContents, new UTF8Encoding(false));
        var modulePath = await WorkspaceCMakeIntegrationHarness.ConfigureBuildAndAssertAsync(session);

        return new WorkspaceScenarioEntry(
            session.WorkspaceRoot,
            session.ManifestPath,
            session.Manifest.WorkspaceId,
            session.Manifest.Name,
            moduleName,
            $"{moduleName}::SampleComponent",
            moveSpeed,
            AssetRelativePath,
            assetFileId,
            assetContents,
            modulePath);
    }

    static WorkspaceLifecycleService CreateLifecycle(string scenarioRoot)
    {
        var serializer = new WorkspaceManifestSerializer();
        var generatedState = new WorkspaceGeneratedProjectStateService();
        var generator = new WorkspaceProjectGenerator(generatedState);
        return new WorkspaceLifecycleService(
            serializer,
            new WorkspaceTemplateService(serializer, generator),
            new RecentWorkspaceStore(GetRecentsPath(scenarioRoot)),
            generatedState);
    }

    static string GetRecentsPath(string scenarioRoot)
        => Path.Combine(scenarioRoot, ".test-state", "recents.yaml");

    static Task<WorkspaceActivationResult> ActivateAsync(
        WorkspaceActivationCoordinator coordinator,
        WorkspaceLifecycleService lifecycle,
        WorkspaceScenarioEntry workspace)
        => coordinator.ActivateAsync(new WorkspaceActivationRequest(
            workspace.ModuleName,
            async cancellationToken =>
            {
                var prepared = await lifecycle.PrepareOpenAsync(
                    workspace.ManifestPath,
                    cancellationToken);
                if (!prepared.Succeeded || prepared.Preparation is null)
                {
                    throw new InvalidOperationException(
                        prepared.Error ??
                        $"Workspace preflight failed: {string.Join("; ", prepared.Validation.Issues.Select(x => x.Message))}");
                }

                try
                {
                    var session = prepared.Preparation.Session;
                    if (session.GeneratedProjectState.Status != WorkspaceGeneratedProjectStateStatus.Current)
                    {
                        throw new InvalidOperationException(
                            $"Generated project state for '{workspace.ModuleName}' is {session.GeneratedProjectState.Status}: " +
                            session.GeneratedProjectState.Guidance);
                    }

                    var launchContext = EngineLaunchContract.Resolve(
                        session.WorkspaceRoot,
                        session.ManifestPath,
                        session.ContentDirectory,
                        session.CacheDirectory,
                        session.WorkspaceRoot,
                        session.Manifest.WorkspaceId);
                    return new WorkspaceActivationCandidate(prepared.Preparation, launchContext);
                }
                catch
                {
                    prepared.Preparation.Discard();
                    throw;
                }
            }));

    static void AssertActive(
        WorkspaceActivationResult result,
        HeadlessWorkspaceActivationOperations operations,
        WorkspaceScenarioEntry expected,
        WorkspaceScenarioEntry inactive,
        long expectedGeneration,
        EditorTypeCacheStatus expectedCacheStatus)
    {
        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(expectedGeneration, result.State.Generation);
        Assert.Equal(WorkspaceActivationPhase.Ready, result.State.Phase);
        Assert.NotNull(operations.Active);
        var active = operations.Active!;
        Assert.Equal(expected.WorkspaceRoot, active.Session.WorkspaceRoot);
        Assert.Equal(expected.ManifestPath, active.LaunchContext.WorkspaceManifestPath);
        Assert.Equal(expected.WorkspaceId, active.LaunchContext.WorkspaceIdentity);
        Assert.Equal(expected.ModuleName, active.Catalog.Document.ModuleName);
        Assert.Equal(expected.ComponentTypeName, Assert.Single(active.Catalog.GetComponentTypeNames()));
        Assert.DoesNotContain(inactive.ComponentTypeName, active.Catalog.GetComponentTypeNames());
        var defaultObject = Assert.Single(
            active.Catalog.Document.Cdos,
            value => string.Equals(value.Typename, expected.ComponentTypeName, StringComparison.Ordinal));
        Assert.True(defaultObject.DefaultValues.TryGetValue("moveSpeed", out var rawMoveSpeed));
        Assert.Equal(
            expected.MoveSpeed,
            Convert.ToSingle(rawMoveSpeed, System.Globalization.CultureInfo.InvariantCulture));
        Assert.Equal(expected.AssetContents, Encoding.UTF8.GetString(active.AssetBytes));
        Assert.NotEqual(inactive.AssetContents, Encoding.UTF8.GetString(active.AssetBytes));
        Assert.Contains(expected.AssetFileId, Encoding.UTF8.GetString(active.AssetBytes), StringComparison.Ordinal);
        Assert.DoesNotContain(inactive.AssetFileId, Encoding.UTF8.GetString(active.AssetBytes), StringComparison.Ordinal);
        Assert.Equal(expectedCacheStatus, active.CacheStatusBeforeRefresh);
        Assert.Equal(expected.ModulePath, active.ModulePath);
        Assert.Equal(expected.WorkspaceRoot, result.Candidate!.LaunchContext.WorkspaceRoot);
    }

    static Dictionary<string, string> HashFiles(string root, IEnumerable<string> relativePaths)
        => relativePaths.ToDictionary(
            relativePath => relativePath,
            relativePath => HashFile(ResolveRelativePath(root, relativePath)),
            StringComparer.Ordinal);

    static string BuildAssetContents(string fileId, long assetImportTime)
        => string.Join('\n',
        [
            "assetInfoType: Sailor::AssetInfo",
            $"fileId: \"{fileId}\"",
            "filename: Identity",
            $"assetImportTime: {assetImportTime}",
            string.Empty
        ]);

    static void AssertHashes(string root, IReadOnlyDictionary<string, string> expectedHashes)
    {
        foreach (var (relativePath, expectedHash) in expectedHashes.OrderBy(x => x.Key, StringComparer.Ordinal))
            Assert.Equal(expectedHash, HashFile(ResolveRelativePath(root, relativePath)));
    }

    static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    static string ResolveRelativePath(string root, string relativePath)
    {
        var resolved = Path.GetFullPath(Path.Combine(
            root,
            relativePath.Replace('/', Path.DirectorySeparatorChar)));
        WorkspacePathPolicy.EnsureInsideRoot(root, resolved, nameof(relativePath));
        return resolved;
    }

    static async Task PersistScenarioDescriptorLastAsync(
        string scenarioRoot,
        ScenarioDescriptor descriptor)
    {
        var descriptorPath = Path.Combine(scenarioRoot, ScenarioDescriptorFileName);
        var temporaryPath = descriptorPath + ".tmp";
        var json = JsonSerializer.Serialize(descriptor, new JsonSerializerOptions { WriteIndented = true });
        await File.WriteAllTextAsync(temporaryPath, json, new UTF8Encoding(false));
        File.Move(temporaryPath, descriptorPath);
    }

    static async Task<ScenarioDescriptor> LoadScenarioDescriptorAsync(string scenarioRoot)
    {
        var descriptorPath = Path.Combine(scenarioRoot, ScenarioDescriptorFileName);
        if (!File.Exists(descriptorPath))
            throw new FileNotFoundException("Phase 1 did not persist the workspace E2E descriptor.", descriptorPath);

        var descriptor = JsonSerializer.Deserialize<ScenarioDescriptor>(
            await File.ReadAllTextAsync(descriptorPath));
        return descriptor ?? throw new InvalidDataException("The workspace E2E descriptor is empty.");
    }

    static void ValidateDescriptor(ScenarioDescriptor descriptor, string scenarioRoot)
    {
        Assert.Equal(ScenarioSchemaVersion, descriptor.SchemaVersion);
        Assert.Equal(scenarioRoot, Path.GetFullPath(descriptor.ScenarioRoot));
        Assert.Equal(WorkspaceAModuleName, descriptor.WorkspaceA.ModuleName);
        Assert.Equal(WorkspaceBModuleName, descriptor.WorkspaceB.ModuleName);
        Assert.Equal(AssetRelativePath, descriptor.WorkspaceA.AssetRelativePath);
        Assert.Equal(AssetRelativePath, descriptor.WorkspaceB.AssetRelativePath);
        Assert.Equal(WorkspaceAAssetFileId, descriptor.WorkspaceA.AssetFileId);
        Assert.Equal(WorkspaceBAssetFileId, descriptor.WorkspaceB.AssetFileId);
        ValidateWorkspaceDescriptor(descriptor.WorkspaceA, scenarioRoot);
        ValidateWorkspaceDescriptor(descriptor.WorkspaceB, scenarioRoot);
        Assert.NotEqual(descriptor.WorkspaceA.WorkspaceRoot, descriptor.WorkspaceB.WorkspaceRoot);
        Assert.Equal(ProtectedWorkspaceAFiles.Length, descriptor.WorkspaceAProtectedFileHashes.Count);
        Assert.All(
            ProtectedWorkspaceAFiles,
            relativePath => Assert.True(
                descriptor.WorkspaceAProtectedFileHashes.ContainsKey(relativePath),
                $"Descriptor does not contain a hash for '{relativePath}'."));
    }

    static void ValidateWorkspaceDescriptor(
        WorkspaceScenarioEntry workspace,
        string scenarioRoot)
    {
        WorkspacePathPolicy.EnsureInsideRoot(
            scenarioRoot,
            Path.GetFullPath(workspace.WorkspaceRoot),
            nameof(workspace.WorkspaceRoot));
        WorkspacePathPolicy.EnsureInsideRoot(
            workspace.WorkspaceRoot,
            Path.GetFullPath(workspace.ManifestPath),
            nameof(workspace.ManifestPath));
        WorkspacePathPolicy.EnsureInsideRoot(
            workspace.WorkspaceRoot,
            Path.GetFullPath(workspace.ModulePath),
            nameof(workspace.ModulePath));
        _ = ResolveRelativePath(workspace.WorkspaceRoot, workspace.AssetRelativePath);
    }

    static string GetScenarioRoot()
    {
        var root = Path.GetFullPath(
            WorkspaceCMakeIntegrationHarness.GetRequiredEnvironmentVariable(
                ScenarioRootEnvironmentVariable));
        var directoryName = Path.GetFileName(root.TrimEnd(
            Path.DirectorySeparatorChar,
            Path.AltDirectorySeparatorChar));
        if (!directoryName.Contains("sailor", StringComparison.OrdinalIgnoreCase) ||
            !directoryName.Contains("e2e", StringComparison.OrdinalIgnoreCase) ||
            Directory.GetParent(root) is null)
        {
            throw new InvalidOperationException(
                $"{ScenarioRootEnvironmentVariable} must name a dedicated Sailor E2E directory, not '{root}'.");
        }

        return root;
    }

    static void ResetScenarioRoot(string scenarioRoot)
    {
        DeleteScenarioRoot(scenarioRoot);
        Directory.CreateDirectory(scenarioRoot);
    }

    static void DeleteScenarioRoot(string scenarioRoot)
    {
        if (Directory.Exists(scenarioRoot))
            Directory.Delete(scenarioRoot, recursive: true);
    }

    public sealed record ScenarioDescriptor(
        int SchemaVersion,
        string ScenarioRoot,
        WorkspaceScenarioEntry WorkspaceA,
        WorkspaceScenarioEntry WorkspaceB,
        Dictionary<string, string> WorkspaceAProtectedFileHashes);

    public sealed record WorkspaceScenarioEntry(
        string WorkspaceRoot,
        string ManifestPath,
        string WorkspaceId,
        string ExpectedSavedName,
        string ModuleName,
        string ComponentTypeName,
        float MoveSpeed,
        string AssetRelativePath,
        string AssetFileId,
        string AssetContents,
        string ModulePath);

    sealed record ActiveWorkspaceSnapshot(
        WorkspaceSession Session,
        EngineLaunchContext LaunchContext,
        string ModulePath,
        EditorTypeCatalogSnapshot Catalog,
        byte[] AssetBytes,
        EditorTypeCacheStatus CacheStatusBeforeRefresh);

    sealed class ConfiguredWorkspaceTemplateService : WorkspaceTemplateService
    {
        readonly WorkspaceManifestSerializer _serializer;
        readonly WorkspaceProjectGenerator _generator;
        readonly string _moduleName;

        public ConfiguredWorkspaceTemplateService(
            WorkspaceManifestSerializer serializer,
            WorkspaceProjectGenerator generator,
            string moduleName)
            : base(serializer, generator)
        {
            _serializer = serializer;
            _generator = generator;
            _moduleName = moduleName;
        }

        public override async Task<WorkspaceSession> CreateAsync(
            WorkspaceCreateRequest request,
            CancellationToken cancellationToken = default)
        {
            var workspaceRoot = Path.GetFullPath(request.WorkspaceDirectory);
            var manifestPath = string.IsNullOrWhiteSpace(request.ManifestPath)
                ? Path.Combine(workspaceRoot, ManifestFileName)
                : Path.GetFullPath(request.ManifestPath);
            var manifest = WorkspaceManifest.CreateDefault(
                request.Name,
                request.EnginePath,
                request.WorkspaceId,
                request.EngineReferenceKind) with
            {
                LogicModuleName = _moduleName
            };
            var session = CreateSession(workspaceRoot, manifest, manifestPath);
            WorkspaceProjectGenerationResult? generated = null;
            try
            {
                generated = await _generator.GenerateAsync(session, cancellationToken);
                await _serializer.SaveAsync(manifestPath, manifest, cancellationToken);
                return session;
            }
            catch
            {
                if (File.Exists(manifestPath))
                    File.Delete(manifestPath);
                if (generated is not null)
                    WorkspaceProjectGenerator.Rollback(generated);
                throw;
            }
        }
    }

    sealed class HeadlessWorkspaceActivationOperations : IWorkspaceActivationOperations, IDisposable
    {
        const uint NativeSuccess = 0;
        const uint NativeBufferTooSmall = 2;
        const string MetadataEntryPoint = "SailorGetWorkspaceTypeMetadataV1";

        static readonly object DllSearchPathLock = new();
        static readonly UTF8Encoding StrictUtf8 = new(false, true);

        readonly WorkspaceLifecycleService _lifecycle;
        readonly string _installPrefix;
        readonly string _vcpkgRoot;
        readonly string _assetRelativePath;
        readonly EditorTypeCacheStore _cacheStore = new();
        nint _moduleHandle;
        long _clearedGeneration = -1;
        bool _disposed;

        public HeadlessWorkspaceActivationOperations(
            WorkspaceLifecycleService lifecycle,
            string installPrefix,
            string vcpkgRoot,
            string assetRelativePath)
        {
            _lifecycle = lifecycle;
            _installPrefix = installPrefix;
            _vcpkgRoot = vcpkgRoot;
            _assetRelativePath = assetRelativePath;
        }

        public List<string> Events { get; } = [];
        public ActiveWorkspaceSnapshot? Active { get; private set; }
        public int UnloadCount { get; private set; }

        public Task StopAsync(CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Events.Add("stop");
            if (_moduleHandle != nint.Zero)
            {
                NativeLibrary.Free(_moduleHandle);
                _moduleHandle = nint.Zero;
                UnloadCount++;
            }

            return Task.CompletedTask;
        }

        public Task ClearAsync(long generation, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_moduleHandle != nint.Zero)
                throw new InvalidOperationException("The prior workspace DLL was not unloaded before clear.");

            Events.Add($"clear:{generation}");
            Active = null;
            _clearedGeneration = generation;
            return Task.CompletedTask;
        }

        public async Task CommitAsync(
            WorkspaceActivationCandidate candidate,
            CancellationToken cancellationToken)
        {
            if (Active is not null || _moduleHandle != nint.Zero)
                throw new InvalidOperationException("Prior workspace state survived the clear boundary.");

            Events.Add($"commit:{candidate.Session.Manifest.LogicModuleName}");
            await _lifecycle.CommitActivationAsync(candidate.Preparation, cancellationToken);
        }

        public Task StartAsync(
            EngineLaunchContext launchContext,
            long generation,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_clearedGeneration != generation)
                throw new InvalidOperationException("The workspace start did not follow its clear generation.");
            if (_moduleHandle != nint.Zero || Active is not null)
                throw new InvalidOperationException("Workspace state was already active at start.");

            var session = _lifecycle.Current
                ?? throw new InvalidOperationException("The lifecycle did not commit the activation candidate.");
            if (!string.Equals(
                    session.WorkspaceRoot,
                    launchContext.WorkspaceRoot,
                    WorkspacePathPolicy.PathComparison) ||
                !string.Equals(
                    session.ManifestPath,
                    launchContext.WorkspaceManifestPath,
                    WorkspacePathPolicy.PathComparison) ||
                !string.Equals(session.Manifest.WorkspaceId, launchContext.WorkspaceIdentity, StringComparison.Ordinal))
            {
                throw new InvalidOperationException("The committed lifecycle session does not match the launch context.");
            }

            Events.Add($"start:{session.Manifest.LogicModuleName}:{generation}");
            var modulePath = WorkspaceCMakeIntegrationHarness.GetReleaseModulePath(session);
            try
            {
                var loaded = LoadMetadata(modulePath);
                _moduleHandle = loaded.Handle;
                var liveCatalog = EditorTypeCatalogSnapshot.Parse(loaded.Metadata);
                if (!string.Equals(
                        liveCatalog.Document.ModuleName,
                        session.Manifest.LogicModuleName,
                        StringComparison.Ordinal))
                {
                    throw new InvalidDataException(
                        $"DLL metadata module '{liveCatalog.Document.ModuleName}' does not match '{session.Manifest.LogicModuleName}'.");
                }

                var expectedComponent = $"{session.Manifest.LogicModuleName}::SampleComponent";
                if (!liveCatalog.GetComponentTypeNames().SequenceEqual([expectedComponent], StringComparer.Ordinal))
                {
                    throw new InvalidDataException(
                        $"DLL metadata did not expose exactly '{expectedComponent}'.");
                }

                var assetPath = ResolveRelativePath(launchContext.ContentDirectory, _assetRelativePath);
                var assetBytes = File.ReadAllBytes(assetPath);
                var cacheIdentity = new EditorTypeCacheIdentity(
                    launchContext.WorkspaceIdentity,
                    $"installed:{Path.GetFullPath(_installPrefix)}",
                    $"sha256:{HashFile(modulePath)}",
                    $"workspace-module:{session.Manifest.LogicModuleName}");
                var cacheBefore = _cacheStore.Load(
                    launchContext.EditorTypesCacheFilePath,
                    cacheIdentity);
                if (cacheBefore.Status == EditorTypeCacheStatus.Missing)
                {
                    var write = _cacheStore.Save(
                        launchContext.EditorTypesCacheFilePath,
                        cacheIdentity,
                        loaded.Metadata);
                    if (!write.Succeeded)
                        throw new InvalidDataException(write.Diagnostic);
                }
                else if (cacheBefore.Status == EditorTypeCacheStatus.Loaded)
                {
                    AssertEquivalentMetadata(
                        liveCatalog,
                        EditorTypeCatalogSnapshot.Parse(cacheBefore.Payload!));
                }
                else if (EditorTypeCacheStore.ShouldInvalidate(cacheBefore.Status))
                {
                    var invalidation = _cacheStore.Invalidate(launchContext.EditorTypesCacheFilePath);
                    if (!invalidation.Succeeded)
                        throw new InvalidDataException(invalidation.Diagnostic);
                    var write = _cacheStore.Save(
                        launchContext.EditorTypesCacheFilePath,
                        cacheIdentity,
                        loaded.Metadata);
                    if (!write.Succeeded)
                        throw new InvalidDataException(write.Diagnostic);
                }
                else
                {
                    throw new InvalidDataException(cacheBefore.Diagnostic);
                }

                var persistedCache = _cacheStore.Load(
                    launchContext.EditorTypesCacheFilePath,
                    cacheIdentity);
                if (!persistedCache.Succeeded)
                    throw new InvalidDataException(persistedCache.Diagnostic);
                AssertEquivalentMetadata(
                    liveCatalog,
                    EditorTypeCatalogSnapshot.Parse(persistedCache.Payload!));

                Active = new ActiveWorkspaceSnapshot(
                    session,
                    launchContext,
                    modulePath,
                    liveCatalog,
                    assetBytes,
                    cacheBefore.Status);
                return Task.CompletedTask;
            }
            catch
            {
                if (_moduleHandle != nint.Zero)
                {
                    NativeLibrary.Free(_moduleHandle);
                    _moduleHandle = nint.Zero;
                    UnloadCount++;
                }

                throw;
            }
        }

        (nint Handle, string Metadata) LoadMetadata(string modulePath)
        {
            var searchDirectories = new[]
            {
                Path.GetDirectoryName(modulePath)!,
                Path.Combine(_installPrefix, "bin"),
                Path.Combine(_vcpkgRoot, "installed", "x64-windows", "bin")
            }
                .Where(Directory.Exists)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToArray();

            lock (DllSearchPathLock)
            {
                var previousPath = Environment.GetEnvironmentVariable("PATH");
                Environment.SetEnvironmentVariable(
                    "PATH",
                    string.Join(
                        Path.PathSeparator,
                        searchDirectories.Append(previousPath ?? string.Empty)));
                nint handle = nint.Zero;
                try
                {
                    handle = NativeLibrary.Load(modulePath);
                    var metadata = ReadMetadata(handle, modulePath);
                    return (handle, metadata);
                }
                catch
                {
                    if (handle != nint.Zero)
                        NativeLibrary.Free(handle);
                    throw;
                }
                finally
                {
                    Environment.SetEnvironmentVariable("PATH", previousPath);
                }
            }
        }

        static string ReadMetadata(nint handle, string modulePath)
        {
            var export = NativeLibrary.GetExport(handle, MetadataEntryPoint);
            var getMetadata = Marshal.GetDelegateForFunctionPointer<GetWorkspaceTypeMetadataV1>(export);
            var queryResult = getMetadata(nint.Zero, 0, out var payloadSize);
            if (queryResult != NativeBufferTooSmall || payloadSize == 0 || payloadSize > int.MaxValue)
            {
                throw new InvalidDataException(
                    $"Metadata size query failed for '{modulePath}' with result {queryResult} and size {payloadSize}.");
            }

            var buffer = Marshal.AllocHGlobal(checked((int)payloadSize));
            try
            {
                var metadataResult = getMetadata(buffer, payloadSize, out var writtenSize);
                if (metadataResult != NativeSuccess || writtenSize != payloadSize)
                {
                    throw new InvalidDataException(
                        $"Metadata read failed for '{modulePath}' with result {metadataResult}; " +
                        $"expected {payloadSize} bytes and received {writtenSize}.");
                }

                var bytes = new byte[checked((int)writtenSize)];
                Marshal.Copy(buffer, bytes, 0, bytes.Length);
                return StrictUtf8.GetString(bytes);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        static void AssertEquivalentMetadata(
            EditorTypeCatalogSnapshot live,
            EditorTypeCatalogSnapshot cached)
        {
            if (!string.Equals(
                    live.Document.ModuleName,
                    cached.Document.ModuleName,
                    StringComparison.Ordinal) ||
                !live.GetComponentTypeNames().SequenceEqual(
                    cached.GetComponentTypeNames(),
                    StringComparer.Ordinal) ||
                !GetDefaultSignatures(live).SequenceEqual(
                    GetDefaultSignatures(cached),
                    StringComparer.Ordinal))
            {
                throw new InvalidDataException("The persisted editor type cache does not match live DLL metadata.");
            }
        }

        static IReadOnlyList<string> GetDefaultSignatures(EditorTypeCatalogSnapshot catalog)
            => catalog.Document.Cdos
                .OrderBy(value => value.Typename, StringComparer.Ordinal)
                .Select(value =>
                    $"{value.Typename}:" +
                    string.Join(
                        ",",
                        value.DefaultValues
                            .OrderBy(property => property.Key, StringComparer.Ordinal)
                            .Select(property =>
                                $"{property.Key}={Convert.ToString(property.Value, System.Globalization.CultureInfo.InvariantCulture)}")))
                .ToArray();

        public void Dispose()
        {
            if (_disposed)
                return;

            _disposed = true;
            Active = null;
            if (_moduleHandle != nint.Zero)
            {
                NativeLibrary.Free(_moduleHandle);
                _moduleHandle = nint.Zero;
                UnloadCount++;
            }
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate uint GetWorkspaceTypeMetadataV1(
            nint destination,
            ulong destinationCapacity,
            out ulong payloadSize);
    }
}
