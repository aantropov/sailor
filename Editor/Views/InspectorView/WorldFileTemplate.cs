using SailorEditor.Content;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Globalization;
using System.Numerics;

namespace SailorEditor;

public sealed class WorldFileTemplate : DataTemplate
{
    public WorldFileTemplate()
    {
        LoadTemplate = static () => new GlobalIlluminationEditorPanel();
    }
}

sealed class GlobalIlluminationEditorPanel : VerticalStackLayout
{
    sealed class BindingDraft
    {
        public string Name { get; set; } = string.Empty;
        public FileId Asset { get; set; } = new();
        public GlobalIlluminationCompositionMode Mode { get; set; }
        public float InitialWeight { get; set; }
        public bool Preload { get; set; }
    }

    readonly EngineService engineService;
    readonly WorldService worldService;
    readonly AssetsService assetsService;
    readonly List<BindingDraft> bindings = [];
    readonly Observable<FileId> layoutSource = new(new FileId());
    readonly IDispatcherTimer statusTimer;

    WorldFile? worldFile;
    VerticalStackLayout? bindingsHost;
    Label? runtimeStatus;
    Label? bakeStatus;
    ProgressBar? bakeProgress;
    Button? bakeNewButton;
    Button? bakeLayoutButton;
    Button? cancelBakeButton;
    Entry? stateNameEntry;
    Entry? outputPathEntry;
    Entry? raysEntry;
    Entry? bouncesEntry;
    Entry? seedEntry;
    Entry? subdivisionEntry;
    Entry? spacingEntry;
    Entry? normalBiasEntry;
    Entry? viewBiasEntry;
    Entry? maxDistanceEntry;
    Entry? boundsMinEntry;
    Entry? boundsMaxEntry;
    Entry? environmentEntry;
    CheckBox? includeSky;
    CheckBox? includeEmissive;
    CheckBox? includeDirect;
    CheckBox? autoBounds;
    CheckBox? overwrite;
    Picker? bakedMode;
    Entry? bakedWeight;
    CheckBox? bakedPreload;
    bool subscribed;
    bool polling;
    bool handledSuccess;

    public GlobalIlluminationEditorPanel()
    {
        Spacing = 10;
        engineService = MauiProgram.GetService<EngineService>();
        worldService = MauiProgram.GetService<WorldService>();
        assetsService = MauiProgram.GetService<AssetsService>();
        statusTimer = Dispatcher.CreateTimer();
        statusTimer.Interval = TimeSpan.FromMilliseconds(300);
        statusTimer.Tick += PollStatus;
        BindingContextChanged += (_, _) => Bind(BindingContext as WorldFile);
        HandlerChanged += (_, _) => UpdateSubscription();
    }

    void UpdateSubscription()
    {
        if (Handler is not null && !subscribed)
        {
            worldService.OnUpdateWorldAction += OnWorldUpdated;
            subscribed = true;
        }
        else if (Handler is null && subscribed)
        {
            worldService.OnUpdateWorldAction -= OnWorldUpdated;
            subscribed = false;
            statusTimer.Stop();
        }
    }

    void OnWorldUpdated(ViewModels.World _)
    {
        if (IsCurrentWorld())
        {
            LoadBindings();
            RebuildBindingRows();
        }
    }

    void Bind(WorldFile? value)
    {
        worldFile = value;
        handledSuccess = false;
        Build();
    }

    bool IsCurrentWorld() =>
        worldFile?.FileId is not null &&
        !worldFile.FileId.IsEmpty() &&
        worldService.CurrentWorldAsset?.FileId is not null &&
        worldFile.FileId == worldService.CurrentWorldAsset.FileId;

    void Build()
    {
        statusTimer.Stop();
        Children.Clear();
        if (worldFile is null)
            return;

        Children.Add(new Views.ControlPanelView { BindingContext = worldFile });
        Children.Add(Section("Global Illumination ECS"));
        if (!IsCurrentWorld())
        {
            Children.Add(new Label
            {
                Text = "Open this saved level to edit or bake its probe states.",
                FontSize = 12,
                TextColor = Color.FromArgb("#D4A72C")
            });
            Children.Add(ActionButton("Open Level", OpenLevelAsync));
            return;
        }

        LoadBindings();
        Children.Add(BuildBindingsCard());
        Children.Add(BuildBakeCard());
        runtimeStatus = new Label
        {
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        };
        Children.Add(runtimeStatus);
        statusTimer.Start();
        _ = RefreshRuntimeStateAsync();
    }

    async Task OpenLevelAsync()
    {
        if (worldFile is not null && await worldService.LoadWorldAsync(worldFile))
        {
            Build();
        }
    }

    void LoadBindings()
    {
        bindings.Clear();
        foreach (var pair in worldService.Current.GlobalIllumination.Probes
            .OrderBy(static pair => pair.Key, StringComparer.Ordinal))
        {
            var source = pair.Value;
            bindings.Add(new BindingDraft
            {
                Name = pair.Key,
                Asset = new FileId(source.Asset?.FileId?.Value ?? string.Empty),
                Mode = source.Mode == GlobalIlluminationProbeMode.Additive
                    ? GlobalIlluminationCompositionMode.Additive
                    : GlobalIlluminationCompositionMode.Blend,
                InitialWeight = source.InitialWeight,
                Preload = source.Preload
            });
        }
    }

    View BuildBindingsCard()
    {
        var card = Card();
        var header = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Auto)
            }
        };
        header.Add(new Label
        {
            Text = "Level Probe States",
            FontAttributes = FontAttributes.Bold,
            VerticalTextAlignment = TextAlignment.Center
        });
        var add = new Button { Text = "Add", HeightRequest = 32 };
        add.Clicked += (_, _) =>
        {
            bindings.Add(new BindingDraft
            {
                Name = UniqueStateName("State"),
                Mode = GlobalIlluminationCompositionMode.Blend
            });
            RebuildBindingRows();
        };
        header.Add(add, 1);
        card.Children.Add(header);
        card.Children.Add(new Label
        {
            Text = "Each asset is one baked state. Blend states form a normalized base; Additive states are accumulated with raw weights.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        });
        bindingsHost = new VerticalStackLayout { Spacing = 8 };
        card.Children.Add(bindingsHost);
        RebuildBindingRows();

        var actions = new HorizontalStackLayout { Spacing = 6 };
        actions.Children.Add(ActionButton("Apply", ApplyBindingsAsync));
        actions.Children.Add(ActionButton("Apply + Save", async () =>
        {
            if (await ApplyBindingsAsync())
                await worldService.SaveCurrentWorldAsync(confirmExisting: false);
        }));
        actions.Children.Add(ActionButton("Refresh Runtime", RefreshRuntimeStateAsync));
        card.Children.Add(actions);
        return card;
    }

    void RebuildBindingRows()
    {
        if (bindingsHost is null)
            return;
        bindingsHost.Children.Clear();
        if (bindings.Count == 0)
        {
            bindingsHost.Children.Add(new Label
            {
                Text = "No baked probe states are bound. Environment irradiance is used as fallback.",
                FontSize = 11,
                TextColor = Color.FromArgb("#929AA5")
            });
            return;
        }

        foreach (var draft in bindings)
        {
            var row = new VerticalStackLayout { Spacing = 5 };
            var title = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition(GridLength.Star),
                    new ColumnDefinition(GridLength.Auto)
                }
            };
            var name = TextEntry(draft.Name, value => draft.Name = value.Trim());
            name.Placeholder = "State name";
            title.Add(name);
            var remove = new Button { Text = "Remove", HeightRequest = 32 };
            remove.Clicked += (_, _) =>
            {
                bindings.Remove(draft);
                RebuildBindingRows();
            };
            title.Add(remove, 1);
            row.Children.Add(title);

            row.Children.Add(Labeled(
                ".probes asset",
                Templates.FileIdEditor<BindingDraft>(
                    draft,
                    nameof(BindingDraft.Asset),
                    static value => value.Asset,
                    static (value, asset) => value.Asset = asset,
                    typeof(ProbeVolumeFile))));

            var mode = new Picker
            {
                ItemsSource = Enum.GetValues<GlobalIlluminationCompositionMode>(),
                SelectedItem = draft.Mode,
                HorizontalOptions = LayoutOptions.Fill
            };
            mode.SelectedIndexChanged += (_, _) =>
            {
                if (mode.SelectedItem is GlobalIlluminationCompositionMode selected)
                    draft.Mode = selected;
            };
            row.Children.Add(Labeled("Composition", mode));
            row.Children.Add(Labeled(
                "Initial weight",
                FloatEntry(draft.InitialWeight, value => draft.InitialWeight = value)));
            row.Children.Add(Labeled(
                "Preload",
                BoolEditor(draft.Preload, value => draft.Preload = value)));
            CardWrap(row, bindingsHost);
        }
    }

    async Task<bool> ApplyBindingsAsync()
    {
        try
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            var descriptors = new List<GlobalIlluminationBindingDescriptor>();
            foreach (var draft in bindings)
            {
                var name = draft.Name.Trim();
                if (string.IsNullOrEmpty(name) || !names.Add(name))
                    throw new InvalidOperationException("Probe state names must be non-empty and unique.");
                if (draft.Asset is null || draft.Asset.IsEmpty())
                    throw new InvalidOperationException($"Probe state '{name}' has no .probes asset.");
                if (!float.IsFinite(draft.InitialWeight) || draft.InitialWeight < 0.0f)
                    throw new InvalidOperationException($"Probe state '{name}' has an invalid weight.");
                descriptors.Add(new GlobalIlluminationBindingDescriptor(
                    name,
                    draft.Asset,
                    draft.Mode,
                    draft.InitialWeight,
                    draft.Preload));
            }

            if (!await engineService.SetGlobalIlluminationSettingsAsync(descriptors))
            {
                SetRuntimeStatus("Global Illumination ECS rejected the level bindings.", true);
                return false;
            }
            if (worldService.CurrentWorldAsset is not null)
                worldService.CurrentWorldAsset.IsDirty = true;
            if (worldFile is not null)
                worldFile.IsDirty = true;
            await RefreshRuntimeStateAsync();
            return true;
        }
        catch (Exception exception)
        {
            SetRuntimeStatus(exception.Message, true);
            return false;
        }
    }

    View BuildBakeCard()
    {
        var card = Card();
        card.Children.Add(new Label
        {
            Text = "Bake One Probe State",
            FontAttributes = FontAttributes.Bold
        });
        card.Children.Add(new Label
        {
            Text = "The native CPU path tracer bakes a single immutable lighting state in the background. Save the level before baking.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        });

        stateNameEntry = TextEntry("Day");
        outputPathEntry = TextEntry(DefaultOutputPath("Day"));
        card.Children.Add(Labeled("State name", stateNameEntry));
        card.Children.Add(Labeled("Output (Content-relative)", outputPathEntry));
        card.Children.Add(Labeled(
            "Reuse layout from",
            Templates.FileIdEditor<Observable<FileId>>(
                layoutSource,
                nameof(Observable<FileId>.Value),
                static value => value.Value,
                static (value, asset) => value.Value = asset,
                typeof(ProbeVolumeFile))));

        bakedMode = new Picker
        {
            ItemsSource = Enum.GetValues<GlobalIlluminationCompositionMode>(),
            SelectedItem = GlobalIlluminationCompositionMode.Blend
        };
        bakedWeight = FloatEntry(1.0f);
        bakedPreload = new CheckBox { IsChecked = true };
        card.Children.Add(Labeled("Binding mode", bakedMode));
        card.Children.Add(Labeled("Binding weight", bakedWeight));
        card.Children.Add(Labeled("Binding preload", bakedPreload));

        raysEntry = UIntEntry(256);
        bouncesEntry = UIntEntry(3);
        seedEntry = UIntEntry(0);
        subdivisionEntry = UIntEntry(3);
        spacingEntry = FloatEntry(1.0f);
        normalBiasEntry = FloatEntry(0.05f);
        viewBiasEntry = FloatEntry(0.05f);
        maxDistanceEntry = FloatEntry(1000.0f);
        includeSky = new CheckBox { IsChecked = true };
        includeEmissive = new CheckBox { IsChecked = true };
        includeDirect = new CheckBox { IsChecked = true };
        card.Children.Add(Labeled("Rays / probe", raysEntry));
        card.Children.Add(Labeled("Indirect bounces", bouncesEntry));
        card.Children.Add(Labeled("Random seed", seedEntry));
        card.Children.Add(Labeled("Max subdivision", subdivisionEntry));
        card.Children.Add(Labeled("Min probe spacing", spacingEntry));
        card.Children.Add(Labeled("Normal bias", normalBiasEntry));
        card.Children.Add(Labeled("View bias", viewBiasEntry));
        card.Children.Add(Labeled("Max ray distance", maxDistanceEntry));
        card.Children.Add(Labeled("Include sky", includeSky));
        card.Children.Add(Labeled("Include emissive", includeEmissive));
        card.Children.Add(Labeled("Include direct lighting", includeDirect));

        autoBounds = new CheckBox { IsChecked = true };
        boundsMinEntry = TextEntry("-10, -10, -10");
        boundsMaxEntry = TextEntry("10, 10, 10");
        environmentEntry = TextEntry("0.03, 0.03, 0.03");
        overwrite = new CheckBox();
        card.Children.Add(Labeled("Automatic bounds", autoBounds));
        card.Children.Add(Labeled("Manual bounds min", boundsMinEntry));
        card.Children.Add(Labeled("Manual bounds max", boundsMaxEntry));
        card.Children.Add(Labeled("Fallback environment", environmentEntry));
        card.Children.Add(Labeled("Overwrite existing file", overwrite));

        var actions = new HorizontalStackLayout { Spacing = 6 };
        bakeNewButton = ActionButton("Bake New", () => StartBakeAsync(false));
        bakeLayoutButton = ActionButton("Bake Using Layout", () => StartBakeAsync(true));
        cancelBakeButton = ActionButton("Cancel", CancelBakeAsync);
        cancelBakeButton.IsEnabled = false;
        actions.Children.Add(bakeNewButton);
        actions.Children.Add(bakeLayoutButton);
        actions.Children.Add(cancelBakeButton);
        card.Children.Add(actions);

        bakeProgress = new ProgressBar { Progress = 0.0 };
        bakeStatus = new Label
        {
            Text = "Idle",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        };
        card.Children.Add(bakeProgress);
        card.Children.Add(bakeStatus);
        return card;
    }

    async Task StartBakeAsync(bool reuseLayout)
    {
        try
        {
            if (!IsCurrentWorld() || worldService.CurrentWorldAsset?.FileId is null)
                throw new InvalidOperationException("Open and save the target level before baking.");
            if (reuseLayout && (layoutSource.Value is null || layoutSource.Value.IsEmpty()))
                throw new InvalidOperationException("Select a .probes layout source first.");

            var stateName = stateNameEntry!.Text?.Trim() ?? string.Empty;
            var outputPath = outputPathEntry!.Text?.Trim() ?? string.Empty;
            if (string.IsNullOrEmpty(stateName) || string.IsNullOrEmpty(outputPath))
                throw new InvalidOperationException("State name and output path are required.");

            var settings = new ProbeVolumeBakeSettings(
                ReadUInt(
                    raysEntry!,
                    "Rays / probe",
                    positive: true,
                    maximum: ProbeVolumeBakeSettings.MaximumRaysPerProbe),
                ReadUInt(
                    bouncesEntry!,
                    "Indirect bounces",
                    positive: true,
                    maximum: ProbeVolumeBakeSettings.MaximumBounceCount),
                ReadUInt(seedEntry!, "Random seed", positive: false),
                ReadUInt(
                    subdivisionEntry!,
                    "Max subdivision",
                    positive: false,
                    maximum: ProbeVolumeBakeSettings.MaximumSubdivisionLevel),
                ReadFloat(spacingEntry!, "Min probe spacing", positive: true),
                ReadFloat(normalBiasEntry!, "Normal bias", positive: false),
                ReadFloat(viewBiasEntry!, "View bias", positive: false),
                ReadFloat(maxDistanceEntry!, "Max ray distance", positive: true),
                includeSky!.IsChecked,
                includeEmissive!.IsChecked,
                includeDirect!.IsChecked);
            var request = new ProbeVolumeBakeRequest(
                worldService.CurrentWorldAsset.FileId,
                outputPath,
                stateName,
                settings,
                reuseLayout ? layoutSource.Value : null,
                autoBounds!.IsChecked,
                ReadVector(boundsMinEntry!, "Manual bounds min"),
                ReadVector(boundsMaxEntry!, "Manual bounds max"),
                ReadVector(environmentEntry!, "Fallback environment"),
                overwrite!.IsChecked);
            handledSuccess = false;
            if (!await engineService.StartProbeVolumeBakeAsync(request))
                throw new InvalidOperationException("The native bake controller rejected the request.");
            SetBakeRunning(true);
            await PollStatusAsync();
        }
        catch (Exception exception)
        {
            SetBakeStatus(exception.Message, true);
            SetBakeRunning(false);
        }
    }

    async Task CancelBakeAsync()
    {
        try
        {
            await engineService.CancelProbeVolumeBakeAsync();
        }
        catch (Exception exception)
        {
            SetBakeStatus(exception.Message, true);
        }
    }

    async void PollStatus(object? sender, EventArgs arguments)
    {
        await PollStatusAsync();
    }

    async Task PollStatusAsync()
    {
        if (polling || !IsCurrentWorld())
            return;
        polling = true;
        try
        {
            var status = await engineService.GetProbeVolumeBakeStatusAsync();
            if (status is null)
                return;
            SetBakeRunning(status.IsRunning);
            if (bakeProgress is not null)
                bakeProgress.Progress = Math.Clamp(status.Progress, 0.0f, 1.0f);
            var counts = status.TotalProbes > 0
                ? $" {status.CompletedProbes}/{status.TotalProbes} probes"
                : string.Empty;
            SetBakeStatus(
                $"{status.State}: {status.Stage}{counts} ({status.ElapsedSeconds:0.0}s)" +
                (string.IsNullOrWhiteSpace(status.Diagnostic)
                    ? string.Empty
                    : $"{Environment.NewLine}{status.Diagnostic}"),
                status.State == ProbeVolumeBakeLifecycleState.Failed);
            if (status.State == ProbeVolumeBakeLifecycleState.Succeeded &&
                !handledSuccess)
            {
                handledSuccess = true;
                await CompleteBakeAsync(status);
            }
        }
        catch (Exception exception)
        {
            SetBakeStatus(exception.Message, true);
        }
        finally
        {
            polling = false;
        }
    }

    async Task CompleteBakeAsync(ProbeVolumeBakeStatus status)
    {
        if (!await engineService.RequestAssetReloadAsync())
            throw new InvalidOperationException("The bake succeeded, but the asset registry reload failed.");
        await assetsService.RefreshAsync();
        var physicalPath = Path.GetFullPath(Path.Combine(
            assetsService.CurrentProjectRootPath,
            status.OutputVirtualPath));
        var asset = assetsService.Files
            .OfType<ProbeVolumeFile>()
            .FirstOrDefault(candidate =>
                candidate.Asset is not null &&
                ProjectContentPathPolicy.IsSamePath(
                    candidate.Asset.FullName,
                    physicalPath));
        if (asset is null)
            throw new InvalidOperationException("The baked .probes file was not discovered as an asset.");

        var name = stateNameEntry!.Text?.Trim() ?? string.Empty;
        var existing = bindings.FirstOrDefault(binding =>
            string.Equals(binding.Name, name, StringComparison.Ordinal));
        existing ??= new BindingDraft { Name = name };
        if (!bindings.Contains(existing))
            bindings.Add(existing);
        existing.Asset = asset.FileId;
        existing.Mode = bakedMode!.SelectedItem is GlobalIlluminationCompositionMode mode
            ? mode
            : GlobalIlluminationCompositionMode.Blend;
        existing.InitialWeight = ReadFloat(
            bakedWeight!,
            "Binding weight",
            positive: false);
        existing.Preload = bakedPreload!.IsChecked;
        RebuildBindingRows();
        if (!await ApplyBindingsAsync())
            throw new InvalidOperationException("The bake succeeded, but the level binding was rejected.");
        var save = await worldService.SaveCurrentWorldAsync(confirmExisting: false);
        if (save.Outcome != SceneSaveOutcome.Saved)
            throw new InvalidOperationException(save.Error ?? "The baked binding could not be saved to the level.");
        SetBakeStatus(
            $"Succeeded: {status.ProbeCount} probes in {status.BrickCount} bricks. Bound '{name}' and saved the level.",
            false);
    }

    async Task RefreshRuntimeStateAsync()
    {
        try
        {
            var state = await engineService.GetGlobalIlluminationStateAsync();
            if (state is null)
                return;
            var probes = state.Probes.Count == 0
                ? "no states"
                : string.Join(
                    ", ",
                    state.Probes.Select(probe =>
                        $"{probe.Name}={probe.Weight:0.###} {probe.Mode}/{probe.Residency}"));
            SetRuntimeStatus(
                $"Budget {state.MaxProbeStatesPerSnapshot}; {probes}. {state.Diagnostic}",
                state.Probes.Any(probe => probe.Residency == GlobalIlluminationResidency.Failed));
        }
        catch (Exception exception)
        {
            SetRuntimeStatus(exception.Message, true);
        }
    }

    void SetBakeRunning(bool running)
    {
        if (bakeNewButton is not null)
            bakeNewButton.IsEnabled = !running;
        if (bakeLayoutButton is not null)
            bakeLayoutButton.IsEnabled = !running;
        if (cancelBakeButton is not null)
            cancelBakeButton.IsEnabled = running;
    }

    void SetBakeStatus(string text, bool error)
    {
        if (bakeStatus is null)
            return;
        bakeStatus.Text = text;
        bakeStatus.TextColor = error
            ? Colors.IndianRed
            : Color.FromArgb("#929AA5");
    }

    void SetRuntimeStatus(string text, bool error)
    {
        if (runtimeStatus is null)
            return;
        runtimeStatus.Text = text;
        runtimeStatus.TextColor = error
            ? Colors.IndianRed
            : Color.FromArgb("#929AA5");
    }

    string UniqueStateName(string prefix)
    {
        for (var suffix = 1; ; ++suffix)
        {
            var candidate = suffix == 1 ? prefix : $"{prefix}{suffix}";
            if (bindings.All(binding =>
                !string.Equals(binding.Name, candidate, StringComparison.Ordinal)))
                return candidate;
        }
    }

    string DefaultOutputPath(string state) =>
        $"Lighting/{Path.GetFileNameWithoutExtension(worldFile?.Asset?.Name) ?? "Level"}_{state}.probes";

    static uint ReadUInt(
        Entry entry,
        string label,
        bool positive,
        uint maximum = uint.MaxValue)
    {
        if (!uint.TryParse(entry.Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ||
            (positive && value == 0) ||
            value > maximum)
        {
            var range = positive ? $"1..{maximum}" : $"0..{maximum}";
            throw new InvalidOperationException($"{label} must be in the range {range}.");
        }
        return value;
    }

    static float ReadFloat(Entry entry, string label, bool positive)
    {
        if (!float.TryParse(entry.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ||
            !float.IsFinite(value) ||
            (positive ? value <= 0.0f : value < 0.0f))
            throw new InvalidOperationException($"{label} is invalid.");
        return value;
    }

    static Vector3 ReadVector(Entry entry, string label)
    {
        var parts = (entry.Text ?? string.Empty).Split(
            ',',
            StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length != 3 ||
            !float.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var x) ||
            !float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var y) ||
            !float.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var z) ||
            !float.IsFinite(x) || !float.IsFinite(y) || !float.IsFinite(z))
            throw new InvalidOperationException($"{label} must contain three finite comma-separated values.");
        return new Vector3(x, y, z);
    }

    static VerticalStackLayout Card() => new()
    {
        BackgroundColor = Color.FromArgb("#1D2229"),
        Padding = 10,
        Spacing = 7
    };

    static void CardWrap(View view, VerticalStackLayout host)
    {
        host.Children.Add(new Border
        {
            Stroke = Color.FromArgb("#3B424D"),
            Padding = 8,
            Content = view
        });
    }

    static Label Section(string text) => new()
    {
        Text = text,
        FontAttributes = FontAttributes.Bold,
        FontSize = 16
    };

    static Grid Labeled(string label, View editor)
    {
        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(140),
                new ColumnDefinition(GridLength.Star)
            },
            ColumnSpacing = 6
        };
        grid.Add(new Label
        {
            Text = label,
            VerticalTextAlignment = TextAlignment.Center,
            FontSize = 12
        });
        grid.Add(editor, 1);
        return grid;
    }

    static Entry TextEntry(string value, Action<string>? changed = null)
    {
        var entry = new Entry { Text = value, ReturnType = ReturnType.Done };
        if (changed is not null)
            entry.Unfocused += (_, _) => changed(entry.Text ?? string.Empty);
        return entry;
    }

    static Entry UIntEntry(uint value) => new()
    {
        Text = value.ToString(CultureInfo.InvariantCulture),
        Keyboard = Keyboard.Numeric,
        ReturnType = ReturnType.Done
    };

    static Entry FloatEntry(float value, Action<float>? changed = null)
    {
        var entry = new Entry
        {
            Text = value.ToString(CultureInfo.InvariantCulture),
            Keyboard = Keyboard.Numeric,
            ReturnType = ReturnType.Done
        };
        if (changed is not null)
        {
            entry.Unfocused += (_, _) =>
            {
                if (float.TryParse(
                    entry.Text,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var parsed) &&
                    float.IsFinite(parsed))
                    changed(parsed);
            };
        }
        return entry;
    }

    static CheckBox BoolEditor(bool value, Action<bool> changed)
    {
        var checkBox = new CheckBox { IsChecked = value };
        checkBox.CheckedChanged += (_, args) => changed(args.Value);
        return checkBox;
    }

    static Button ActionButton(string text, Func<Task> action)
    {
        var button = new Button { Text = text, HeightRequest = 32 };
        button.Clicked += async (_, _) =>
        {
            button.IsEnabled = false;
            try
            {
                await action();
            }
            finally
            {
                if (button.Handler is not null)
                    button.IsEnabled = true;
            }
        };
        return button;
    }

    static Button ActionButton(string text, Func<Task<bool>> action) =>
        ActionButton(text, async () => { _ = await action(); });
}
