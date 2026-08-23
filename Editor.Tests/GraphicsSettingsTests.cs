using SailorEditor.Settings;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Editor.Tests;

public sealed class GraphicsSettingsTests
{
    [Fact]
    public void Defaults_UseTheApprovedFivePresetContract()
    {
        var project = GraphicsSettingsDefaults.Project;

        Assert.Equal(GraphicsQualityLevel.High, project.Graphics.DefaultQuality);
        Assert.Equal(120, project.Graphics.Presets.Ultra.FpsCap);
        Assert.Equal(8, project.Graphics.Presets.Ultra.MsaaSamples);
        Assert.Equal(1.25, project.Graphics.Presets.Ultra.ShadowBias);
        Assert.Equal([4096, 2048, 2048, 1024], project.Graphics.Presets.Ultra.ShadowCascadeResolutions);
        Assert.Equal(0.85, project.Graphics.Presets.Medium.ResolutionFactor);
        Assert.Equal(GraphicsShadowQuality.Medium, project.Graphics.Presets.Medium.ShadowQuality);
        Assert.Equal(65536, project.Graphics.Presets.Ultra.VegetationInstanceBudget);
        Assert.Equal(32768, project.Graphics.Presets.High.VegetationInstanceBudget);
        Assert.Equal(2048, project.Graphics.Presets.VeryLow.VegetationInstanceBudget);
        Assert.Equal(0.125, project.Graphics.Presets.VeryLow.CloudsResolutionMultiplier);
        Assert.Equal(2, project.Graphics.Presets.VeryLow.LodBias);
        Assert.Equal(4, project.Graphics.Presets.Ultra.MaxGiProbeStatesPerSnapshot);
        Assert.Equal(3, project.Graphics.Presets.High.MaxGiProbeStatesPerSnapshot);
        Assert.Equal(1, project.Graphics.Presets.VeryLow.MaxGiProbeStatesPerSnapshot);
        Assert.True(GraphicsSettingsValidator.Validate(project).IsValid);
        Assert.True(GraphicsSettingsValidator.Validate(GraphicsSettingsDefaults.Editor).IsValid);

        var yaml = GraphicsSettingsYamlCodec.SerializeProject(project);
        Assert.Contains("defaultQuality: High", yaml);
        Assert.Contains("fpsCap: 120", yaml);
        Assert.Contains("shadowBias: 1.25", yaml);
        Assert.Contains("VeryLow:", yaml);
        Assert.Contains("supportSoftShadows: true", yaml);
        Assert.Contains("vegetationInstanceBudget: 32768", yaml);
        Assert.Contains("maxGiProbeStatesPerSnapshot: 4", yaml);
    }

    [Fact]
    public void LegacyProjectSettings_MigrateWithQualitySpecificGiBudgets()
    {
        var root = LoadRoot(
            GraphicsSettingsYamlCodec.SerializeProject(
                GraphicsSettingsDefaults.Project));
        root.Children[new YamlScalarNode("settingsVersion")] =
            new YamlScalarNode("1");
        var graphics = Assert.IsType<YamlMappingNode>(
            root.Children[new YamlScalarNode("graphics")]);
        var presets = Assert.IsType<YamlMappingNode>(
            graphics.Children[new YamlScalarNode("presets")]);
        foreach (var quality in Enum.GetValues<GraphicsQualityLevel>())
        {
            var preset = Assert.IsType<YamlMappingNode>(
                presets.Children[new YamlScalarNode(quality.ToString())]);
            preset.Children.Remove(
                new YamlScalarNode("maxGiProbeStatesPerSnapshot"));
        }
        var diagnostics = new List<string>();

        var migrated = GraphicsSettingsYamlCodec.ParseProject(
            Save(root),
            diagnostics,
            "legacy project fixture");

        Assert.Equal(ProjectSettingsDocument.CurrentVersion, migrated.SettingsVersion);
        Assert.Equal(4, migrated.Graphics.Presets.Ultra.MaxGiProbeStatesPerSnapshot);
        Assert.Equal(1, migrated.Graphics.Presets.VeryLow.MaxGiProbeStatesPerSnapshot);
        Assert.Contains(diagnostics, value =>
            value.Contains("migrated settingsVersion 1 to 2", StringComparison.Ordinal));
    }

    [Fact]
    public void Validate_ReportsEveryGraphicsConstraintWithQualifiedPaths()
    {
        var invalidPreset = GraphicsSettingsDefaults.Presets.High with
        {
            ResolutionFactor = 0.1,
            FpsCap = 0,
            MsaaSamples = 3,
            ShadowQuality = (GraphicsShadowQuality)99,
            ShadowBias = 17.0,
            ShadowCascadeCount = 3,
            ShadowCascadeResolutions = [1024, 300],
            CloudsResolutionMultiplier = 3.0,
            SkyResolution = 96,
            VegetationInstanceBudget = 1048577,
            LodBias = 9,
            MaxGiProbeStatesPerSnapshot = 17
        };
        var document = GraphicsSettingsDefaults.Project with
        {
            Graphics = GraphicsSettingsDefaults.Project.Graphics with
            {
                Presets = GraphicsSettingsDefaults.Presets.With(
                    GraphicsQualityLevel.High,
                    invalidPreset)
            }
        };

        var result = GraphicsSettingsValidator.Validate(document);

        Assert.False(result.IsValid);
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.resolutionFactor");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.fpsCap");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.msaaSamples");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.shadowQuality");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.shadowBias");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.shadowCascadeResolutions");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.cloudsResolutionMultiplier");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.skyResolution");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.vegetationInstanceBudget");
        Assert.Contains(result.Issues, x => x.Path == "graphics.presets.High.lodBias");
        Assert.Contains(
            result.Issues,
            x => x.Path == "graphics.presets.High.maxGiProbeStatesPerSnapshot");
    }

    [Fact]
    public async Task LegacyPresetWithoutVegetationBudget_UsesItsQualityDefault()
    {
        using var workspace = TempWorkspace.Create();
        var root = LoadRoot(
            GraphicsSettingsYamlCodec.SerializeProject(GraphicsSettingsDefaults.Project));
        var graphics = Assert.IsType<YamlMappingNode>(
            root.Children[new YamlScalarNode("graphics")]);
        var presets = Assert.IsType<YamlMappingNode>(
            graphics.Children[new YamlScalarNode("presets")]);
        var medium = Assert.IsType<YamlMappingNode>(
            presets.Children[new YamlScalarNode("Medium")]);
        medium.Children.Remove(new YamlScalarNode("vegetationInstanceBudget"));
        await File.WriteAllTextAsync(workspace.Paths.ProjectSettingsPath, Save(root));

        var snapshot = await new GraphicsSettingsService(() => workspace.Paths)
            .EnsureLoadedAsync();

        Assert.Equal(16384, snapshot.Project.Graphics.Presets.Medium.VegetationInstanceBudget);
        Assert.DoesNotContain(
            snapshot.Diagnostics,
            value => value.Contains("vegetationInstanceBudget", StringComparison.Ordinal));
    }

    [Fact]
    public async Task ApplyAsync_PreservesUnknownYamlKeysAndUsesAtomicReplacement()
    {
        using var workspace = TempWorkspace.Create();
        var paths = workspace.Paths;
        Directory.CreateDirectory(paths.CacheDirectory);
        var root = LoadRoot(GraphicsSettingsYamlCodec.SerializeProject(GraphicsSettingsDefaults.Project));
        root.Add("pluginSettings", new YamlMappingNode("enabled", "true"));
        var graphics = Assert.IsType<YamlMappingNode>(root.Children[new YamlScalarNode("graphics")]);
        graphics.Add("futureRenderer", "meshlets");
        var presets = Assert.IsType<YamlMappingNode>(graphics.Children[new YamlScalarNode("presets")]);
        var ultra = Assert.IsType<YamlMappingNode>(presets.Children[new YamlScalarNode("Ultra")]);
        ultra.Add("futureUltraOption", "kept");
        await File.WriteAllTextAsync(paths.ProjectSettingsPath, Save(root));

        var restartCount = 0;
        var service = new GraphicsSettingsService(
            () => paths,
            _ =>
            {
                restartCount++;
                return Task.FromResult(true);
            });
        var loaded = await service.EnsureLoadedAsync();
        var edited = loaded.Project with
        {
            Graphics = loaded.Project.Graphics with
            {
                Presets = loaded.Project.Graphics.Presets.With(
                    GraphicsQualityLevel.Ultra,
                    loaded.Project.Graphics.Presets.Ultra with
                    {
                        ResolutionFactor = 0.95,
                        ShadowBias = 1.25,
                        MaxGiProbeStatesPerSnapshot = 2
                    })
            }
        };

        var result = await service.ApplyAsync(edited, loaded.Editor, loaded);

        Assert.True(result.ProjectChanged);
        Assert.True(result.QualityChanged);
        Assert.True(result.EngineRestarted);
        Assert.Equal(1, restartCount);
        Assert.False(File.Exists(paths.EditorSettingsPath));
        Assert.Empty(Directory.EnumerateFiles(paths.WorkspaceRoot, "*.tmp", SearchOption.AllDirectories));

        var saved = LoadRoot(await File.ReadAllTextAsync(paths.ProjectSettingsPath));
        Assert.True(saved.Children.ContainsKey(new YamlScalarNode("pluginSettings")));
        var savedGraphics = Assert.IsType<YamlMappingNode>(saved.Children[new YamlScalarNode("graphics")]);
        Assert.Equal("meshlets", Assert.IsType<YamlScalarNode>(savedGraphics.Children[new YamlScalarNode("futureRenderer")]).Value);
        var savedPresets = Assert.IsType<YamlMappingNode>(savedGraphics.Children[new YamlScalarNode("presets")]);
        var savedUltra = Assert.IsType<YamlMappingNode>(savedPresets.Children[new YamlScalarNode("Ultra")]);
        Assert.Equal("kept", Assert.IsType<YamlScalarNode>(savedUltra.Children[new YamlScalarNode("futureUltraOption")]).Value);
        Assert.Equal("0.95", Assert.IsType<YamlScalarNode>(savedUltra.Children[new YamlScalarNode("resolutionFactor")]).Value);
        Assert.Equal("1.25", Assert.IsType<YamlScalarNode>(savedUltra.Children[new YamlScalarNode("shadowBias")]).Value);
        Assert.Equal(
            "2",
            Assert.IsType<YamlScalarNode>(
                savedUltra.Children[new YamlScalarNode(
                    "maxGiProbeStatesPerSnapshot")]).Value);
    }

    [Fact]
    public async Task EditorChanges_RestartQualityAndApplyStatsLive()
    {
        using var workspace = TempWorkspace.Create(customCache: true);
        await File.WriteAllTextAsync(
            workspace.Paths.ProjectSettingsPath,
            GraphicsSettingsYamlCodec.SerializeProject(GraphicsSettingsDefaults.Project));
        Directory.CreateDirectory(workspace.Paths.CacheDirectory);
        var editorRoot = LoadRoot(
            GraphicsSettingsYamlCodec.SerializeEditor(GraphicsSettingsDefaults.Editor));
        editorRoot.Add("pluginState", "preserved");
        var editorGraphics = Assert.IsType<YamlMappingNode>(
            editorRoot.Children[new YamlScalarNode("graphics")]);
        editorGraphics.Add("futureOverlay", "preserved");
        await File.WriteAllTextAsync(
            workspace.Paths.EditorSettingsPath,
            Save(editorRoot));
        var restartCount = 0;
        var appliedStats = new List<GraphicsStatsMode>();
        var published = new List<GraphicsSettingsSnapshot>();
        var service = new GraphicsSettingsService(
            () => workspace.Paths,
            _ =>
            {
                restartCount++;
                return Task.FromResult(true);
            },
            (mode, _) =>
            {
                appliedStats.Add(mode);
                return Task.FromResult(true);
            });
        service.SettingsChanged += (_, snapshot) => published.Add(snapshot);

        var qualityResult = await service.SetSelectedQualityAsync(EditorQualitySelection.Ultra);
        var statsResult = await service.SetStatsModeAsync(GraphicsStatsMode.RenderStatsAndQueries);

        Assert.True(qualityResult.QualityChanged);
        Assert.True(qualityResult.EngineRestarted);
        Assert.True(statsResult.StatsChanged);
        Assert.True(statsResult.StatsAppliedLive);
        Assert.Equal(1, restartCount);
        Assert.Equal([GraphicsStatsMode.RenderStatsAndQueries], appliedStats);
        Assert.Contains(published, snapshot =>
            snapshot.Editor.Graphics.SelectedQuality ==
                EditorQualitySelection.Ultra);
        Assert.Equal(
            GraphicsStatsMode.RenderStatsAndQueries,
            published[^1].Editor.Graphics.StatsMode);
        Assert.Equal(
            Path.Combine(workspace.CustomCacheDirectory, GraphicsSettingsPaths.EditorFileName),
            workspace.Paths.EditorSettingsPath);
        var editorYaml = await File.ReadAllTextAsync(workspace.Paths.EditorSettingsPath);
        Assert.Contains("selectedQuality: Ultra", editorYaml);
        Assert.Contains("statsMode: RenderStatsAndQueries", editorYaml);
        Assert.Contains("settingsVersion: 1", editorYaml);
        var savedEditorRoot = LoadRoot(editorYaml);
        Assert.Equal(
            "preserved",
            Assert.IsType<YamlScalarNode>(
                savedEditorRoot.Children[new YamlScalarNode("pluginState")]).Value);
        var savedEditorGraphics = Assert.IsType<YamlMappingNode>(
            savedEditorRoot.Children[new YamlScalarNode("graphics")]);
        Assert.Equal(
            "preserved",
            Assert.IsType<YamlScalarNode>(
                savedEditorGraphics.Children[new YamlScalarNode("futureOverlay")]).Value);

        var reconnected = await new GraphicsSettingsService(() => workspace.Paths)
            .EnsureLoadedAsync();
        Assert.Equal(
            EditorQualitySelection.Ultra,
            reconnected.Editor.Graphics.SelectedQuality);
        Assert.Equal(
            GraphicsStatsMode.RenderStatsAndQueries,
            reconnected.Editor.Graphics.StatsMode);
    }

    [Fact]
    public async Task EnsureLoadedAsync_FollowsWorkspaceChangesWithoutLeakingOverrides()
    {
        using var first = TempWorkspace.Create();
        using var second = TempWorkspace.Create();
        await File.WriteAllTextAsync(
            first.Paths.ProjectSettingsPath,
            GraphicsSettingsYamlCodec.SerializeProject(GraphicsSettingsDefaults.Project));
        await File.WriteAllTextAsync(
            second.Paths.ProjectSettingsPath,
            GraphicsSettingsYamlCodec.SerializeProject(
                GraphicsSettingsDefaults.Project with
                {
                    Graphics = GraphicsSettingsDefaults.Project.Graphics with
                    {
                        DefaultQuality = GraphicsQualityLevel.Low
                    }
                }));
        Directory.CreateDirectory(first.Paths.CacheDirectory);
        await File.WriteAllTextAsync(
            first.Paths.EditorSettingsPath,
            GraphicsSettingsYamlCodec.SerializeEditor(
                GraphicsSettingsDefaults.Editor with
                {
                    Graphics = GraphicsSettingsDefaults.Editor.Graphics with
                    {
                        SelectedQuality = EditorQualitySelection.Ultra
                    }
                }));
        var activePaths = first.Paths;
        var service = new GraphicsSettingsService(() => activePaths);

        var firstSnapshot = await service.EnsureLoadedAsync();
        activePaths = second.Paths;
        var secondSnapshot = await service.EnsureLoadedAsync();

        Assert.Equal(EditorQualitySelection.Ultra, firstSnapshot.Editor.Graphics.SelectedQuality);
        Assert.Equal(GraphicsQualityLevel.Ultra, firstSnapshot.EffectiveQuality);
        Assert.Equal(EditorQualitySelection.ProjectDefault, secondSnapshot.Editor.Graphics.SelectedQuality);
        Assert.Equal(GraphicsQualityLevel.Low, secondSnapshot.EffectiveQuality);
        Assert.Equal(second.Paths, service.Current!.Paths);

        var staleEdit = firstSnapshot.Project with
        {
            Graphics = firstSnapshot.Project.Graphics with
            {
                DefaultQuality = GraphicsQualityLevel.VeryLow
            }
        };
        var error = await Assert.ThrowsAsync<GraphicsSettingsStaleSnapshotException>(() =>
            service.ApplyAsync(
                staleEdit,
                firstSnapshot.Editor,
                firstSnapshot));
        Assert.Contains("active workspace changed", error.Message, StringComparison.OrdinalIgnoreCase);
        var unchangedSecond = await new GraphicsSettingsService(() => second.Paths)
            .EnsureLoadedAsync();
        Assert.Equal(
            GraphicsQualityLevel.Low,
            unchangedSecond.Project.Graphics.DefaultQuality);
    }

    [Fact]
    public async Task FirstEditorSpecificChange_CreatesResolvedCacheDocumentOnly()
    {
        using var workspace = TempWorkspace.Create(customCache: true);
        await File.WriteAllTextAsync(
            workspace.Paths.ProjectSettingsPath,
            GraphicsSettingsYamlCodec.SerializeProject(GraphicsSettingsDefaults.Project));
        var service = new GraphicsSettingsService(
            () => workspace.Paths,
            applyStatsModeAsync: (_, _) => Task.FromResult(true));

        var loaded = await service.EnsureLoadedAsync();

        Assert.False(File.Exists(loaded.Paths.EditorSettingsPath));
        await service.SetStatsModeAsync(GraphicsStatsMode.RenderStats);
        Assert.True(File.Exists(loaded.Paths.EditorSettingsPath));
        Assert.False(File.Exists(Path.Combine(workspace.Root, GraphicsSettingsPaths.EditorFileName)));
    }

    [Fact]
    public async Task InvalidYaml_ProducesDiagnosticsAndSafeDefaults()
    {
        using var workspace = TempWorkspace.Create();
        await File.WriteAllTextAsync(
            workspace.Paths.ProjectSettingsPath,
            "settingsVersion: 1\ngraphics: [broken");

        var snapshot = await new GraphicsSettingsService(() => workspace.Paths)
            .EnsureLoadedAsync();

        Assert.Equal(GraphicsQualityLevel.High, snapshot.Project.Graphics.DefaultQuality);
        Assert.Equal(EditorQualitySelection.ProjectDefault, snapshot.Editor.Graphics.SelectedQuality);
        Assert.Contains(snapshot.Diagnostics, x => x.Contains("failed to parse YAML", StringComparison.Ordinal));
        Assert.Contains(snapshot.Diagnostics, x => x.Contains("was not found", StringComparison.Ordinal));
    }

    [Fact]
    public async Task EnsureLoadedAsync_DiscardsLoadWhenWorkspaceChangesDuringIo()
    {
        using var first = TempWorkspace.Create();
        using var second = TempWorkspace.Create();
        var activePaths = first.Paths;
        long generation = 1;
        var storage = new BlockingInitialLoadStorage(
            first.Paths,
            second.Paths);
        var published = new List<GraphicsSettingsSnapshot>();
        var service = new GraphicsSettingsService(
            () => activePaths,
            storage: storage,
            workspaceGenerationProvider: () => Volatile.Read(ref generation));
        service.SettingsChanged += (_, snapshot) => published.Add(snapshot);

        var load = service.EnsureLoadedAsync();
        await storage.FirstLoadStarted.Task;
        activePaths = second.Paths;
        Interlocked.Increment(ref generation);
        storage.ContinueFirstLoad.TrySetResult(true);

        var snapshot = await load;

        Assert.Equal(second.Paths, snapshot.Paths);
        Assert.Equal(2, snapshot.WorkspaceGeneration);
        Assert.Equal(GraphicsQualityLevel.Low, snapshot.Project.Graphics.DefaultQuality);
        Assert.DoesNotContain(published, value => value.Paths == first.Paths);
        Assert.Single(published);
        Assert.Same(snapshot, service.Current);
    }

    [Fact]
    public async Task ApplyAsync_DoesNotWritePublishOrRestartWhenQueuedWorkspaceMutationIsStale()
    {
        using var first = TempWorkspace.Create();
        using var second = TempWorkspace.Create();
        var activePaths = first.Paths;
        long generation = 7;
        var storage = new TrackingApplyStorage();
        var serializer = new BlockingMutationSerializer();
        var restartCount = 0;
        var published = new List<GraphicsSettingsSnapshot>();
        var service = new GraphicsSettingsService(
            () => activePaths,
            _ =>
            {
                restartCount++;
                return Task.FromResult(true);
            },
            storage: storage,
            workspaceGenerationProvider: () => Volatile.Read(ref generation),
            serializeWorkspaceMutationAsync: serializer.RunAsync);
        service.SettingsChanged += (_, snapshot) => published.Add(snapshot);
        var loaded = await service.EnsureLoadedAsync();
        published.Clear();
        var edited = loaded.Project with
        {
            Graphics = loaded.Project.Graphics with
            {
                DefaultQuality = GraphicsQualityLevel.Ultra
            }
        };

        var apply = service.ApplyAsync(edited, loaded.Editor, loaded);
        await serializer.MutationWaiting.Task;
        activePaths = second.Paths;
        Interlocked.Increment(ref generation);
        serializer.ContinueMutation.TrySetResult(true);

        await Assert.ThrowsAsync<GraphicsSettingsStaleSnapshotException>(
            () => apply);
        Assert.Equal(0, restartCount);
        Assert.Equal(0, storage.ProjectWriteCount);
        Assert.Empty(published);
        Assert.Null(service.Current);
    }

    [Fact]
    public async Task ApplyAsync_RefusesToOverwriteExternalFileRevision()
    {
        using var workspace = TempWorkspace.Create();
        var original = GraphicsSettingsYamlCodec.SerializeProject(
            GraphicsSettingsDefaults.Project);
        await File.WriteAllTextAsync(
            workspace.Paths.ProjectSettingsPath,
            original);
        var service = new GraphicsSettingsService(() => workspace.Paths);
        var loaded = await service.EnsureLoadedAsync();
        var external = GraphicsSettingsYamlCodec.SerializeProject(
            GraphicsSettingsDefaults.Project with
            {
                Graphics = GraphicsSettingsDefaults.Project.Graphics with
                {
                    DefaultQuality = GraphicsQualityLevel.Low
                }
            });
        await File.WriteAllTextAsync(
            workspace.Paths.ProjectSettingsPath,
            external);
        var editorDraft = loaded.Project with
        {
            Graphics = loaded.Project.Graphics with
            {
                DefaultQuality = GraphicsQualityLevel.Ultra
            }
        };

        await Assert.ThrowsAsync<GraphicsSettingsStaleSnapshotException>(() =>
            service.ApplyAsync(editorDraft, loaded.Editor, loaded));

        Assert.Equal(
            external,
            await File.ReadAllTextAsync(workspace.Paths.ProjectSettingsPath));
    }

    [Fact]
    public async Task ApplyAsync_GenerationBoundRestartRejectsWorkspaceSwitchBeforeEffect()
    {
        using var first = TempWorkspace.Create();
        using var second = TempWorkspace.Create();
        await File.WriteAllTextAsync(
            first.Paths.ProjectSettingsPath,
            GraphicsSettingsYamlCodec.SerializeProject(
                GraphicsSettingsDefaults.Project));
        var activePaths = first.Paths;
        long generation = 21;
        var effectWaiting = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var continueEffect = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var restartCount = 0;
        var service = new GraphicsSettingsService(
            () => activePaths,
            workspaceGenerationProvider: () => Volatile.Read(ref generation),
            restartEngineForGenerationAsync: async (expectedGeneration, cancellationToken) =>
            {
                effectWaiting.TrySetResult(true);
                await continueEffect.Task.WaitAsync(cancellationToken);
                if (expectedGeneration != Volatile.Read(ref generation))
                    return false;
                restartCount++;
                return true;
            });
        var loaded = await service.EnsureLoadedAsync();
        var edited = loaded.Project with
        {
            Graphics = loaded.Project.Graphics with
            {
                DefaultQuality = GraphicsQualityLevel.Ultra
            }
        };

        var apply = service.ApplyAsync(edited, loaded.Editor, loaded);
        await effectWaiting.Task;
        activePaths = second.Paths;
        Interlocked.Increment(ref generation);
        continueEffect.TrySetResult(true);

        await Assert.ThrowsAsync<GraphicsSettingsStaleSnapshotException>(
            () => apply);
        Assert.Equal(0, restartCount);
        Assert.Equal(
            GraphicsQualityLevel.Ultra,
            (await new GraphicsSettingsService(() => first.Paths)
                .EnsureLoadedAsync()).Project.Graphics.DefaultQuality);
        Assert.False(File.Exists(second.Paths.ProjectSettingsPath));
    }

    [Fact]
    public async Task ApplyAsync_RefusesMalformedYamlWithoutChangingItsBytes()
    {
        using var workspace = TempWorkspace.Create();
        var malformed = "settingsVersion: 1\ngraphics: [broken\nfuture: keep-this-byte-for-byte\n";
        await File.WriteAllTextAsync(
            workspace.Paths.ProjectSettingsPath,
            malformed);
        var service = new GraphicsSettingsService(() => workspace.Paths);
        var loaded = await service.EnsureLoadedAsync();
        var draft = loaded.Project with
        {
            Graphics = loaded.Project.Graphics with
            {
                DefaultQuality = GraphicsQualityLevel.Ultra
            }
        };

        var error = await Assert.ThrowsAsync<GraphicsSettingsPersistenceException>(
            () => service.ApplyAsync(draft, loaded.Editor, loaded));

        Assert.Contains("malformed", error.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(
            malformed,
            await File.ReadAllTextAsync(workspace.Paths.ProjectSettingsPath));
    }

    [Fact]
    public void DraftSession_PreservesDirtyRawValuesAcrossRefreshAndNavigation()
    {
        using var workspace = TempWorkspace.Create();
        var snapshot = new GraphicsSettingsSnapshot(
            workspace.Paths,
            11,
            GraphicsSettingsDefaults.Project,
            GraphicsSettingsDefaults.Editor,
            GraphicsSettingsFileRevision.Missing,
            GraphicsSettingsFileRevision.Missing,
            []);
        var session = new GraphicsSettingsDraftSession();
        var draft = session.GetOrCreate(snapshot);
        draft.SetSelections(
            GraphicsQualityLevel.Low,
            EditorQualitySelection.VeryLow,
            GraphicsStatsMode.RenderStats);
        draft.SetPreset(
            GraphicsQualityLevel.Ultra,
            draft.GetPreset(GraphicsQualityLevel.Ultra) with
            {
                ResolutionFactor = "unfinished-value",
                ShadowBias = "bias-in-progress",
                MaxGiProbeStatesPerSnapshot = "too-many"
            });

        var afterSearchRefresh = session.GetOrCreate(snapshot);
        var afterCategoryNavigation = session.GetOrCreate(snapshot);

        Assert.Same(draft, afterSearchRefresh);
        Assert.Same(draft, afterCategoryNavigation);
        Assert.True(afterCategoryNavigation.IsDirty);
        Assert.Equal(
            "unfinished-value",
            afterCategoryNavigation
                .GetPreset(GraphicsQualityLevel.Ultra)
                .ResolutionFactor);
        Assert.Equal(
            "bias-in-progress",
            afterCategoryNavigation
                .GetPreset(GraphicsQualityLevel.Ultra)
                .ShadowBias);
        Assert.Equal(
            GraphicsQualityLevel.Low,
            afterCategoryNavigation.ProjectDefaultQuality);
        Assert.False(afterCategoryNavigation.TryBuild(
            out _,
            out _,
            out var issues));
        Assert.Contains(
            issues,
            issue => issue.Path == "graphics.presets.Ultra.resolutionFactor");
        Assert.Contains(
            issues,
            issue => issue.Path == "graphics.presets.Ultra.shadowBias");
        Assert.Contains(
            issues,
            issue => issue.Path ==
                "graphics.presets.Ultra.maxGiProbeStatesPerSnapshot");

        var reverted = session.Replace(snapshot);
        Assert.False(reverted.IsDirty);
        Assert.Equal(
            "1",
            reverted.GetPreset(GraphicsQualityLevel.Ultra).ResolutionFactor);
        Assert.Equal(
            "1.25",
            reverted.GetPreset(GraphicsQualityLevel.Ultra).ShadowBias);
        Assert.Equal(
            "4",
            reverted.GetPreset(GraphicsQualityLevel.Ultra)
                .MaxGiProbeStatesPerSnapshot);
    }

    static YamlMappingNode LoadRoot(string yaml)
    {
        var stream = new YamlStream();
        using var reader = new StringReader(yaml);
        stream.Load(reader);
        return Assert.IsType<YamlMappingNode>(Assert.Single(stream.Documents).RootNode);
    }

    static string Save(YamlMappingNode root)
    {
        using var writer = new StringWriter();
        new YamlStream(new YamlDocument(root)).Save(writer, false);
        return writer.ToString();
    }

    static GraphicsSettingsStorageSnapshot CreateStoredSnapshot(
        ProjectSettingsDocument project,
        WorkspaceEditorSettingsDocument? editor = null,
        string projectRevision = "project-1",
        GraphicsSettingsFileRevision? editorRevision = null)
        => new(
            project,
            editor ?? GraphicsSettingsDefaults.Editor,
            GraphicsSettingsFileRevision.Readable(projectRevision),
            editorRevision ?? GraphicsSettingsFileRevision.Missing,
            []);

    sealed class BlockingInitialLoadStorage : IGraphicsSettingsStorage
    {
        readonly GraphicsSettingsPaths _first;
        readonly GraphicsSettingsPaths _second;
        int _loadCount;

        public BlockingInitialLoadStorage(
            GraphicsSettingsPaths first,
            GraphicsSettingsPaths second)
        {
            _first = first;
            _second = second;
        }

        public TaskCompletionSource<bool> FirstLoadStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<bool> ContinueFirstLoad { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async Task<GraphicsSettingsStorageSnapshot> LoadAsync(
            GraphicsSettingsPaths paths,
            CancellationToken cancellationToken)
        {
            var call = Interlocked.Increment(ref _loadCount);
            if (call == 1)
            {
                Assert.Equal(_first, paths);
                FirstLoadStarted.TrySetResult(true);
                await ContinueFirstLoad.Task.WaitAsync(cancellationToken);
            }
            else
            {
                Assert.Equal(_second, paths);
            }

            var quality = paths == _first
                ? GraphicsQualityLevel.High
                : GraphicsQualityLevel.Low;
            return CreateStoredSnapshot(
                GraphicsSettingsDefaults.Project with
                {
                    Graphics = GraphicsSettingsDefaults.Project.Graphics with
                    {
                        DefaultQuality = quality
                    }
                },
                projectRevision: $"project-{call}");
        }

        public Task ValidateForUpdateAsync(
            string path,
            GraphicsSettingsFileRevision expectedRevision,
            CancellationToken cancellationToken)
            => throw new NotSupportedException();

        public Task<GraphicsSettingsFileRevision> WriteProjectAsync(
            string path,
            ProjectSettingsDocument document,
            GraphicsSettingsFileRevision expectedRevision,
            CancellationToken cancellationToken)
            => throw new NotSupportedException();

        public Task<GraphicsSettingsFileRevision> WriteEditorAsync(
            string path,
            WorkspaceEditorSettingsDocument document,
            GraphicsSettingsFileRevision expectedRevision,
            CancellationToken cancellationToken)
            => throw new NotSupportedException();
    }

    sealed class BlockingMutationSerializer
    {
        public TaskCompletionSource<bool> MutationWaiting { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<bool> ContinueMutation { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async Task RunAsync(
            Func<CancellationToken, Task> operation,
            CancellationToken cancellationToken)
        {
            MutationWaiting.TrySetResult(true);
            await ContinueMutation.Task.WaitAsync(cancellationToken);
            await operation(cancellationToken);
        }
    }

    sealed class TrackingApplyStorage : IGraphicsSettingsStorage
    {
        ProjectSettingsDocument _project = GraphicsSettingsDefaults.Project;
        GraphicsSettingsFileRevision _projectRevision =
            GraphicsSettingsFileRevision.Readable("project-1");
        public int ProjectWriteCount { get; private set; }

        public Task<GraphicsSettingsStorageSnapshot> LoadAsync(
            GraphicsSettingsPaths paths,
            CancellationToken cancellationToken)
            => Task.FromResult(CreateStoredSnapshot(
                _project,
                projectRevision: _projectRevision.ContentHash));

        public Task ValidateForUpdateAsync(
            string path,
            GraphicsSettingsFileRevision expectedRevision,
            CancellationToken cancellationToken)
        {
            if (path.EndsWith(GraphicsSettingsPaths.ProjectFileName, StringComparison.Ordinal))
                Assert.Equal(_projectRevision, expectedRevision);
            else
                Assert.Equal(GraphicsSettingsFileRevision.Missing, expectedRevision);
            return Task.CompletedTask;
        }

        public Task<GraphicsSettingsFileRevision> WriteProjectAsync(
            string path,
            ProjectSettingsDocument document,
            GraphicsSettingsFileRevision expectedRevision,
            CancellationToken cancellationToken)
        {
            Assert.Equal(_projectRevision, expectedRevision);
            ProjectWriteCount++;
            _project = document;
            _projectRevision = GraphicsSettingsFileRevision.Readable("project-2");
            return Task.FromResult(_projectRevision);
        }

        public Task<GraphicsSettingsFileRevision> WriteEditorAsync(
            string path,
            WorkspaceEditorSettingsDocument document,
            GraphicsSettingsFileRevision expectedRevision,
            CancellationToken cancellationToken)
            => throw new NotSupportedException();
    }

    sealed class TempWorkspace : IDisposable
    {
        TempWorkspace(string root, bool customCache)
        {
            Root = root;
            CustomCacheDirectory = customCache
                ? Path.Combine(root, "Intermediate", "Editor Cache")
                : Path.Combine(root, "Cache");
            Directory.CreateDirectory(root);
            Paths = GraphicsSettingsPaths.Create(root, CustomCacheDirectory);
        }

        public string Root { get; }
        public string CustomCacheDirectory { get; }
        public GraphicsSettingsPaths Paths { get; }

        public static TempWorkspace Create(bool customCache = false)
            => new(
                Path.Combine(
                    Path.GetTempPath(),
                    $"sailor-graphics-settings-{Guid.NewGuid():N}"),
                customCache);

        public void Dispose()
        {
            if (Directory.Exists(Root))
                Directory.Delete(Root, recursive: true);
        }
    }
}
