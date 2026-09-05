using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Content;
using SailorEditor.Helpers;
using SailorEditor.Scene;
using SailorEditor.Services;
using SailorEditor.Settings;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Globalization;
using System.Numerics;

namespace SailorEditor.Views;

public sealed class LightingPanelView : ContentView
{
    public LightingPanelView()
    {
        BackgroundColor = Color.FromArgb("#17181B");
        Content = new ScrollView
        {
            Content = new GlobalIlluminationEditorPanel(),
            Orientation = ScrollOrientation.Vertical,
            VerticalOptions = LayoutOptions.Fill,
            HorizontalOptions = LayoutOptions.Fill,
            VerticalScrollBarVisibility = ScrollBarVisibility.Always
        };
    }
}

sealed partial class GlobalIlluminationEditorPanel : VerticalStackLayout
{
    sealed record GlobalIlluminationModeOption(
        GlobalIlluminationRuntimeMode Value,
        string DisplayName)
    {
        public override string ToString() => DisplayName;
    }

    static readonly GlobalIlluminationModeOption[] GlobalIlluminationModes =
    [
        new(GlobalIlluminationRuntimeMode.NoGI, "No GI"),
        new(GlobalIlluminationRuntimeMode.Runtime, "Runtime"),
        new(GlobalIlluminationRuntimeMode.Baked, "Baked")
    ];

    sealed record RuntimeBudgetOption(
        RuntimeGIProbesEditorBudget Value,
        string DisplayName)
    {
        public override string ToString() => DisplayName;
    }

    static readonly RuntimeBudgetOption[] RuntimeBudgets =
    [
        new(RuntimeGIProbesEditorBudget.Eco, "Eco"),
        new(RuntimeGIProbesEditorBudget.Balanced, "Balanced")
    ];

    sealed record RuntimeDebugViewOption(
        RuntimeGIProbesEditorDebugView Value,
        SceneViewRenderMode RenderMode,
        string DisplayName)
    {
        public override string ToString() => DisplayName;
    }

    static readonly RuntimeDebugViewOption[] RuntimeDebugViews =
    [
        new(
            RuntimeGIProbesEditorDebugView.Lit,
            SceneViewRenderMode.Lit,
            "Lit"),
        new(
            RuntimeGIProbesEditorDebugView.IndirectOnly,
            SceneViewRenderMode.GlobalIlluminationOnly,
            "Indirect Only"),
        new(
            RuntimeGIProbesEditorDebugView.Probes,
            SceneViewRenderMode.GlobalIlluminationProbes,
            "Probes"),
        new(
            RuntimeGIProbesEditorDebugView.Bricks,
            SceneViewRenderMode.GlobalIlluminationBricks,
            "Bricks"),
        new(
            RuntimeGIProbesEditorDebugView.Validity,
            SceneViewRenderMode.GlobalIlluminationValidity,
            "Validity"),
        new(
            RuntimeGIProbesEditorDebugView.Visibility,
            SceneViewRenderMode.GlobalIlluminationVisibility,
            "Visibility"),
        new(
            RuntimeGIProbesEditorDebugView.Residency,
            SceneViewRenderMode.GlobalIlluminationResidency,
            "Residency"),
        new(
            RuntimeGIProbesEditorDebugView.Fallback,
            SceneViewRenderMode.GlobalIlluminationFallback,
            "Fallback"),
        new(
            RuntimeGIProbesEditorDebugView.Subdivisions,
            SceneViewRenderMode.GlobalIlluminationSubdivisions,
            "Subdivisions")
    ];

    sealed partial class BindingDraft : ObservableObject
    {
        [ObservableProperty]
        string name = string.Empty;

        [ObservableProperty]
        FileId asset = new();

        [ObservableProperty]
        GlobalIlluminationCompositionMode mode;

        [ObservableProperty]
        string initialWeightText = "0";

        [ObservableProperty]
        bool preload;
    }

    sealed record BindingDraftSnapshot(
        string Name,
        FileId Asset,
        GlobalIlluminationCompositionMode Mode,
        string InitialWeightText,
        bool Preload);

    readonly EngineService engineService;
    readonly WorldService worldService;
    readonly AssetsService assetsService;
    readonly GraphicsSettingsService graphicsSettingsService;
    readonly List<BindingDraft> bindings = [];
    readonly Observable<FileId> layoutSource = new(new FileId());
    readonly GIProbesBakeStatusGate bakeStatusGate = new();
    readonly IDispatcherTimer statusTimer;

    WorldFile? worldFile;
    VerticalStackLayout? bindingsHost;
    Label? runtimeStatus;
    ProgressBar? runtimeProgress;
    ProgressBar? runtimeRefinementProgress;
    Button? runtimePreviewButton;
    Button? runtimePauseButton;
    Picker? runtimeBudget;
    Picker? runtimeDebugView;
    View? bakedBindingsContent;
    View? bakeCard;
    View? runtimeCard;
    Label? bakeStatus;
    ProgressBar? bakeProgress;
    Button? bakeNewButton;
    Button? bakeLayoutButton;
    Button? cancelBakeButton;
    Entry? stateNameEntry;
    Entry? outputPathEntry;
    Entry? raysEntry;
    Entry? bouncesEntry;
    Entry? threadCountEntry;
    Entry? seedEntry;
    Entry? subdivisionEntry;
    Entry? spacingEntry;
    Entry? normalBiasEntry;
    Entry? viewBiasEntry;
    Entry? maxDistanceEntry;
    Entry? environmentEntry;
    CheckBox? includeSky;
    CheckBox? includeEmissive;
    CheckBox? includeDirect;
    CheckBox? overwrite;
    Picker? bakedMode;
    Entry? bakedWeight;
    CheckBox? bakedPreload;
    Picker? globalIlluminationMode;
    Entry? runtimeBouncesEntry;
    Entry? runtimeSpacingEntry;
    Entry? runtimeNormalBiasEntry;
    Entry? runtimeViewBiasEntry;
    Entry? runtimeMaxDistanceEntry;
    CheckBox? runtimeIncludeSky;
    CheckBox? runtimeIncludeEmissive;
    CheckBox? runtimeIncludeDirect;
    GlobalIlluminationRuntimeMode selectedGlobalIlluminationMode =
        GlobalIlluminationRuntimeMode.Baked;
    RuntimeGIProbesRuntimeState? lastRuntimeGIState;
    bool subscribed;
    bool polling;
    bool handledSuccess;
    bool bakeUiOperationActive;
    bool bakeLaunchPending;
    bool updatingRuntimeControls;

    public GlobalIlluminationEditorPanel()
    {
        Spacing = 10;
        Padding = new Thickness(8, 6);
        engineService = MauiProgram.GetService<EngineService>();
        worldService = MauiProgram.GetService<WorldService>();
        assetsService = MauiProgram.GetService<AssetsService>();
        graphicsSettingsService =
            MauiProgram.GetService<GraphicsSettingsService>();
        statusTimer = Dispatcher.CreateTimer();
        statusTimer.Interval = TimeSpan.FromMilliseconds(300);
        statusTimer.Tick += PollStatus;
        worldFile = worldService.CurrentWorldAsset;
        Build();
        HandlerChanged += (_, _) => UpdateSubscription();
    }

    void UpdateSubscription()
    {
        if (Handler is not null && !subscribed)
        {
            worldService.OnUpdateWorldAction += OnWorldUpdated;
            assetsService.Changed += OnAssetsChanged;
            engineService.OnEditorRenderModeChanged +=
                OnEditorRenderModeChanged;
            graphicsSettingsService.SettingsChanged +=
                OnGraphicsSettingsChanged;
            subscribed = true;
            SynchronizeWorld(refreshBindings: false);
            statusTimer.Start();
            _ = RefreshRuntimeStateAsync();
            _ = RefreshRuntimeEditorControlsAsync();
        }
        else if (Handler is null && subscribed)
        {
            worldService.OnUpdateWorldAction -= OnWorldUpdated;
            assetsService.Changed -= OnAssetsChanged;
            engineService.OnEditorRenderModeChanged -=
                OnEditorRenderModeChanged;
            graphicsSettingsService.SettingsChanged -=
                OnGraphicsSettingsChanged;
            subscribed = false;
            statusTimer.Stop();
        }
    }

    void OnWorldUpdated(ViewModels.World _)
    {
        SynchronizeWorld(refreshBindings: !bakeUiOperationActive);
    }

    void OnAssetsChanged()
    {
        SynchronizeWorld(refreshBindings: false);
    }

    void SynchronizeWorld(bool refreshBindings)
    {
        var activeWorld = worldService.CurrentWorldAsset;
        if (!HasSameStableIdentity(worldFile, activeWorld))
        {
            bakeUiOperationActive = false;
            bakeLaunchPending = false;
            worldFile = activeWorld;
            handledSuccess = false;
            Build();
            return;
        }

        worldFile = activeWorld;
        if (refreshBindings)
        {
            LoadBindings();
            RebuildBindingRows();
        }
    }

    static bool HasSameStableIdentity(WorldFile? lhs, WorldFile? rhs)
    {
        if (lhs is null || rhs is null)
            return lhs is null && rhs is null;
        if (lhs.FileId is null || lhs.FileId.IsEmpty() ||
            rhs.FileId is null || rhs.FileId.IsEmpty())
            return ReferenceEquals(lhs, rhs);
        return WorldAssetRebindPolicy.HasSameStableIdentity(
            lhs.FileId.Value,
            rhs.FileId.Value);
    }

    bool IsCurrentWorld()
    {
        var inspectedFileId = worldFile?.FileId;
        var activeFileId = worldService.CurrentWorldAsset?.FileId;
        return inspectedFileId is not null &&
            !inspectedFileId.IsEmpty() &&
            activeFileId is not null &&
            !activeFileId.IsEmpty() &&
            WorldAssetRebindPolicy.HasSameStableIdentity(
                inspectedFileId.Value,
                activeFileId.Value);
    }

    void Build()
    {
        statusTimer.Stop();
        Children.Clear();
        bindingsHost = null;
        runtimeStatus = null;
        runtimeProgress = null;
        runtimeRefinementProgress = null;
        runtimePreviewButton = null;
        runtimePauseButton = null;
        runtimeBudget = null;
        runtimeDebugView = null;
        bakedBindingsContent = null;
        bakeCard = null;
        runtimeCard = null;
        bakeStatus = null;
        bakeProgress = null;
        Children.Add(Section("Global Illumination"));
        if (!IsCurrentWorld())
        {
            Children.Add(new Label
            {
                Text = "Runtime preview works for the active unsaved level. Save it before baking probe assets.",
                FontSize = 12,
                TextColor = Color.FromArgb("#D4A72C")
            });
        }

        LoadBindings();
        runtimeStatus = new Label
        {
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        };
        Children.Add(BuildBindingsCard());
        runtimeCard = BuildRuntimeCard();
        Children.Add(runtimeCard);
        if (IsCurrentWorld())
        {
            bakeCard = BuildBakeCard();
            Children.Add(bakeCard);
        }
        UpdateProviderVisibility();
        if (Handler is not null)
        {
            statusTimer.Start();
            _ = RefreshRuntimeStateAsync();
        }
    }

    void LoadBindings()
    {
        selectedGlobalIlluminationMode =
            worldService.Current.GlobalIllumination.Mode switch
            {
                ViewModels.GlobalIlluminationMode.NoGI =>
                    GlobalIlluminationRuntimeMode.NoGI,
                ViewModels.GlobalIlluminationMode.Runtime =>
                    GlobalIlluminationRuntimeMode.Runtime,
                _ => GlobalIlluminationRuntimeMode.Baked
            };
        if (globalIlluminationMode is not null)
        {
            globalIlluminationMode.SelectedItem =
                GlobalIlluminationModes.Single(option =>
                    option.Value == selectedGlobalIlluminationMode);
        }
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
                InitialWeightText = source.InitialWeight.ToString(
                    CultureInfo.InvariantCulture),
                Preload = source.Preload
            });
        }
    }

    View BuildBindingsCard()
    {
        var card = Card();
        globalIlluminationMode = new Picker
        {
            ItemsSource = GlobalIlluminationModes,
            SelectedItem = GlobalIlluminationModes.Single(option =>
                option.Value == selectedGlobalIlluminationMode),
            HorizontalOptions = LayoutOptions.Fill
        };
        globalIlluminationMode.SelectedIndexChanged += (_, _) =>
        {
            if (globalIlluminationMode.SelectedItem is
                GlobalIlluminationModeOption selected)
            {
                selectedGlobalIlluminationMode = selected.Value;
                UpdateProviderVisibility();
            }
        };
        card.Children.Add(Labeled("GI mode", globalIlluminationMode));
        card.Children.Add(new Label
        {
            Text = "No GI uses environment fallback only. Runtime traces one automatically sized scene-wide probe grid. Baked uses .probes assets. Direct lights stay realtime.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        });
        var bakedContent = new VerticalStackLayout { Spacing = 8 };
        bakedBindingsContent = bakedContent;
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
        bakedContent.Children.Add(header);
        bakedContent.Children.Add(new Label
        {
            Text = "Each asset is one baked state. Blend states form a normalized base; Additive states are accumulated with raw weights.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        });
        bindingsHost = new VerticalStackLayout { Spacing = 8 };
        bakedContent.Children.Add(bindingsHost);
        card.Children.Add(bakedContent);
        RebuildBindingRows();

        var actions = new HorizontalStackLayout { Spacing = 6 };
        actions.Children.Add(ActionButton("Apply", ApplyBindingsAsync));
        actions.Children.Add(ActionButton("Apply + Save", async () =>
        {
            if (await ApplyBindingsAsync())
                await worldService.SaveCurrentWorldAsync(confirmExisting: false);
        }));
        actions.Children.Add(ActionButton(
            "Refresh Runtime",
            () => RefreshRuntimeStateAsync("Refreshing runtime state", "Refreshed")));
        card.Children.Add(actions);
        if (runtimeStatus is not null)
            card.Children.Add(runtimeStatus);
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
                    typeof(GIProbesFile))));

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
                FloatTextEntry(
                    draft.InitialWeightText,
                    value => draft.InitialWeightText = value)));
            row.Children.Add(Labeled(
                "Preload",
                BoolEditor(draft.Preload, value => draft.Preload = value)));
            CardWrap(row, bindingsHost);
        }
    }

    Task<bool> ApplyBindingsAsync() => ApplyBindingsCoreAsync(refreshRuntime: true);

    List<GlobalIlluminationBindingDescriptor> BuildBindingDescriptors()
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
            if (!GlobalIlluminationBindingInputPolicy.TryParseInitialWeight(
                    draft.InitialWeightText,
                    out var initialWeight))
            {
                throw new InvalidOperationException($"Probe state '{name}' has an invalid weight.");
            }
            descriptors.Add(new GlobalIlluminationBindingDescriptor(
                name,
                draft.Asset,
                draft.Mode,
                initialWeight,
                draft.Preload));
        }
        return descriptors;
    }

    async Task<bool> ApplyBindingsCoreAsync(bool refreshRuntime)
    {
        try
        {
            SetRuntimeStatus("Applying Global Illumination settings...", false);
            var descriptors = BuildBindingDescriptors();
            if (!await engineService.SetGISettingsAsync(
                    selectedGlobalIlluminationMode,
                    BuildRuntimeSettingsDescriptor(),
                    descriptors))
            {
                SetRuntimeStatus("Global Illumination ECS rejected the level bindings.", true);
                return false;
            }
            if (worldService.CurrentWorldAsset is not null)
                worldService.CurrentWorldAsset.IsDirty = true;
            if (worldFile is not null)
                worldFile.IsDirty = true;
            if (refreshRuntime)
            {
                await RefreshRuntimeStateAsync(
                    "Reading applied runtime state",
                    "Applied");
            }
            return true;
        }
        catch (Exception exception)
        {
            SetRuntimeStatus(exception.Message, true);
            return false;
        }
    }

    RuntimeGIProbesSettingsDescriptor BuildRuntimeSettingsDescriptor() => new(
        RuntimeGIProbesSettingsDescriptor.CurrentVersion,
        runtimeIncludeSky?.IsChecked ?? true,
        runtimeIncludeEmissive?.IsChecked ?? true,
        runtimeIncludeDirect?.IsChecked ?? true,
        ReadUInt(
            runtimeBouncesEntry!,
            "Runtime indirect bounces",
            positive: true,
            maximum: RuntimeGIProbesSettingsDescriptor.MaximumBounceCount),
        ReadFloat(
            runtimeSpacingEntry!,
            "Runtime minimum probe spacing",
            positive: true),
        ReadFloat(runtimeNormalBiasEntry!, "Runtime normal bias", positive: false),
        ReadFloat(runtimeViewBiasEntry!, "Runtime view bias", positive: false),
        ReadFloat(
            runtimeMaxDistanceEntry!,
            "Runtime maximum ray distance",
            positive: true));

    View BuildRuntimeCard()
    {
        var settings = worldService.Current.GlobalIllumination.RuntimeProbes ??
            new ViewModels.RuntimeGIProbesSettings();
        var card = Card();
        card.Children.Add(new Label
        {
            Text = "Experimental Runtime Probes",
            FontAttributes = FontAttributes.Bold
        });
        card.Children.Add(new Label
        {
            Text = "Builds a transient camera-local probe cache from Static and Stationary contributors. Preview is explicitly opt-in and never writes .probes assets.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        });
        runtimeBouncesEntry = UIntEntry(settings.BounceCount);
        runtimeSpacingEntry = FloatEntry(settings.MinProbeSpacing);
        runtimeNormalBiasEntry = FloatEntry(settings.NormalBias);
        runtimeViewBiasEntry = FloatEntry(settings.ViewBias);
        runtimeMaxDistanceEntry = FloatEntry(settings.MaxRayDistance);
        runtimeIncludeSky = new CheckBox { IsChecked = settings.IncludeSky };
        runtimeIncludeEmissive = new CheckBox
        {
            IsChecked = settings.IncludeEmissive
        };
        runtimeIncludeDirect = new CheckBox
        {
            IsChecked = settings.IncludeDirectLighting
        };
        card.Children.Add(Labeled("Indirect bounces", runtimeBouncesEntry));
        card.Children.Add(Labeled("Minimum probe spacing", runtimeSpacingEntry));
        card.Children.Add(Labeled("Normal bias", runtimeNormalBiasEntry));
        card.Children.Add(Labeled("View bias", runtimeViewBiasEntry));
        card.Children.Add(Labeled("Maximum ray distance", runtimeMaxDistanceEntry));
        card.Children.Add(Labeled("Include sky", runtimeIncludeSky));
        card.Children.Add(Labeled("Include emissive", runtimeIncludeEmissive));
        card.Children.Add(Labeled("Include direct lighting", runtimeIncludeDirect));
        card.Children.Add(new Label
        {
            Text = "Camera source: focused Scene viewport",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5")
        });

        var editorGraphics = graphicsSettingsService.Current?.Editor.Graphics ??
            GraphicsSettingsDefaults.Editor.Graphics;
        runtimeBudget = new Picker
        {
            ItemsSource = RuntimeBudgets,
            SelectedItem = RuntimeBudgets.Single(option =>
                option.Value == editorGraphics.RuntimeGIProbesBudget),
            HorizontalOptions = LayoutOptions.Fill
        };
        runtimeBudget.SelectedIndexChanged += async (_, _) =>
        {
            if (updatingRuntimeControls ||
                runtimeBudget.SelectedItem is not RuntimeBudgetOption selected)
            {
                return;
            }
            await ExecuteRuntimeCommandAsync(
                async () =>
                {
                    if (!await engineService.SetRuntimeGIProbesPreviewBudgetAsync(
                            selected.Value))
                    {
                        return false;
                    }
                    await graphicsSettingsService.SetRuntimeGIProbesBudgetAsync(
                        selected.Value);
                    return true;
                },
                "The engine rejected the Runtime GI preview budget.");
        };
        card.Children.Add(Labeled("Preview budget", runtimeBudget));

        runtimeDebugView = new Picker
        {
            ItemsSource = RuntimeDebugViews,
            SelectedItem = RuntimeDebugViews.Single(option =>
                option.Value == editorGraphics.RuntimeGIProbesDebugView),
            HorizontalOptions = LayoutOptions.Fill
        };
        runtimeDebugView.SelectedIndexChanged += async (_, _) =>
        {
            if (updatingRuntimeControls ||
                runtimeDebugView.SelectedItem is not RuntimeDebugViewOption selected)
            {
                return;
            }
            await ExecuteRuntimeCommandAsync(
                async () =>
                {
                    if (!await engineService.SetEditorRenderModeAsync(
                            selected.RenderMode))
                    {
                        return false;
                    }
                    await graphicsSettingsService.SetRuntimeGIProbesDebugViewAsync(
                        selected.Value);
                    return true;
                },
                "The engine rejected the Runtime GI debug view.");
        };
        card.Children.Add(Labeled("Debug view", runtimeDebugView));

        var actions = new HorizontalStackLayout { Spacing = 6 };
        runtimePreviewButton = ActionButton("Enable Preview", async () =>
        {
            var enabled = !(lastRuntimeGIState?.PreviewEnabled ?? false);
            await ExecuteRuntimeCommandAsync(
                async () =>
                {
                    if (!await engineService.SetRuntimeGIProbesPreviewAsync(
                            enabled))
                    {
                        return false;
                    }
                    await graphicsSettingsService
                        .SetRuntimeGIProbesPreviewEnabledAsync(enabled);
                    return true;
                },
                "The engine rejected the Runtime GI preview state.");
        });
        runtimePauseButton = ActionButton("Pause", async () =>
        {
            var paused = !(lastRuntimeGIState?.Paused ?? false);
            await ExecuteRuntimeCommandAsync(
                () => engineService.SetRuntimeGIProbesPausedAsync(paused),
                "The engine rejected the Runtime GI pause state.");
        });
        actions.Children.Add(runtimePreviewButton);
        actions.Children.Add(runtimePauseButton);
        actions.Children.Add(ActionButton("Restart", async () =>
        {
            await ExecuteRuntimeCommandAsync(
                () => engineService.RestartRuntimeGIProbesAsync(),
                "The engine rejected the Runtime GI restart request.");
        }));
        actions.Children.Add(ActionButton("Rebuild Scene", async () =>
        {
            await ExecuteRuntimeCommandAsync(
                () => engineService.RebuildRuntimeGIProbesSceneAsync(),
                "The engine rejected the Runtime GI scene rebuild request.");
        }));
        card.Children.Add(actions);
        runtimeProgress = new ProgressBar { Progress = 0.0 };
        runtimeRefinementProgress = new ProgressBar { Progress = 0.0 };
        card.Children.Add(Labeled("Coverage", runtimeProgress));
        card.Children.Add(Labeled("Refinement", runtimeRefinementProgress));
        return card;
    }

    void UpdateProviderVisibility()
    {
        var runtime = selectedGlobalIlluminationMode ==
            GlobalIlluminationRuntimeMode.Runtime;
        var baked = selectedGlobalIlluminationMode ==
            GlobalIlluminationRuntimeMode.Baked;
        if (bakedBindingsContent is not null)
            bakedBindingsContent.IsVisible = baked;
        if (bakeCard is not null)
            bakeCard.IsVisible = baked;
        if (runtimeCard is not null)
            runtimeCard.IsVisible = runtime;
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
            Text = "The native CPU path tracer bakes a single immutable lighting state with the selected CPU thread count. Save the level before baking.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            LineBreakMode = LineBreakMode.WordWrap
        });

        var defaultStateName = GIProbesBakeOutputPolicy.FindAvailableStateName(
            "Day",
            bindings.Select(binding => binding.Name),
            StateOutputExists);
        stateNameEntry = TextEntry(defaultStateName);
        outputPathEntry = TextEntry(DefaultOutputPath(defaultStateName));
        card.Children.Add(Labeled("State name", stateNameEntry));
        card.Children.Add(Labeled("Output (Content-relative)", outputPathEntry));
        card.Children.Add(Labeled(
            "Reuse layout from",
            Templates.FileIdEditor<Observable<FileId>>(
                layoutSource,
                nameof(Observable<FileId>.Value),
                static value => value.Value,
                static (value, asset) => value.Value = asset,
                typeof(GIProbesFile))));

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
        threadCountEntry = UIntEntry((uint)Math.Clamp(
            Environment.ProcessorCount / 2,
            1,
            4));
        seedEntry = UIntEntry(0);
        subdivisionEntry = UIntEntry(
            GIProbesBakeSettings.MaximumSubdivisionLevel);
        spacingEntry = FloatEntry(1.0f);
        normalBiasEntry = FloatEntry(0.05f);
        viewBiasEntry = FloatEntry(0.05f);
        maxDistanceEntry = FloatEntry(1000.0f);
        includeSky = new CheckBox { IsChecked = true };
        includeEmissive = new CheckBox { IsChecked = true };
        includeDirect = new CheckBox { IsChecked = true };
        card.Children.Add(Labeled("Rays / probe", raysEntry));
        card.Children.Add(Labeled("Indirect bounces", bouncesEntry));
        card.Children.Add(Labeled("Bake threads", threadCountEntry));
        card.Children.Add(Labeled("Random seed", seedEntry));
        card.Children.Add(Labeled("Subdivision limit", subdivisionEntry));
        card.Children.Add(Labeled("Min probe spacing", spacingEntry));
        card.Children.Add(Labeled("Normal bias", normalBiasEntry));
        card.Children.Add(Labeled("View bias", viewBiasEntry));
        card.Children.Add(Labeled("Max ray distance", maxDistanceEntry));
        card.Children.Add(Labeled("Include sky", includeSky));
        card.Children.Add(Labeled("Include emissive", includeEmissive));
        card.Children.Add(Labeled("Include direct lighting", includeDirect));

        environmentEntry = TextEntry("0.03, 0.03, 0.03");
        overwrite = new CheckBox();
        card.Children.Add(Labeled("Fallback environment", environmentEntry));
        card.Children.Add(Labeled("Overwrite existing file", overwrite));

        var actions = new HorizontalStackLayout { Spacing = 6 };
        bakeNewButton = ActionButton(
            "Bake New",
            () => StartBakeAsync(false),
            restoreEnabledAfterAction: false);
        bakeLayoutButton = ActionButton(
            "Bake Using Layout",
            () => StartBakeAsync(true),
            restoreEnabledAfterAction: false);
        cancelBakeButton = ActionButton(
            "Cancel",
            CancelBakeAsync,
            restoreEnabledAfterAction: false);
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
        var launchStage = "Validating bake request";
        bakeStatusGate.BeginLaunch();
        try
        {
            if (selectedGlobalIlluminationMode !=
                GlobalIlluminationRuntimeMode.Baked)
            {
                throw new InvalidOperationException(
                    "Select Baked GI mode before starting a probe bake.");
            }
            if (!IsCurrentWorld() || worldService.CurrentWorldAsset?.FileId is null)
                throw new InvalidOperationException("Open and save the target level before baking.");
            if (reuseLayout && (layoutSource.Value is null || layoutSource.Value.IsEmpty()))
                throw new InvalidOperationException("Select a .probes layout source first.");

            var stateName = stateNameEntry!.Text?.Trim() ?? string.Empty;
            var outputPath = outputPathEntry!.Text?.Trim() ?? string.Empty;
            if (string.IsNullOrEmpty(stateName) || string.IsNullOrEmpty(outputPath))
                throw new InvalidOperationException("State name and output path are required.");
            if (!GIProbesBakeOutputPolicy.TryResolveWriteTarget(
                    assetsService.CurrentProjectRootPath,
                    outputPath,
                    overwrite!.IsChecked,
                    out _,
                    out var outputError))
            {
                throw new InvalidOperationException(outputError);
            }

            var settings = new GIProbesBakeSettings(
                ReadUInt(
                    raysEntry!,
                    "Rays / probe",
                    positive: true,
                    maximum: GIProbesBakeSettings.MaximumRaysPerProbe),
                ReadUInt(
                    bouncesEntry!,
                    "Indirect bounces",
                    positive: true,
                    maximum: GIProbesBakeSettings.MaximumBounceCount),
                ReadUInt(seedEntry!, "Random seed", positive: false),
                ReadUInt(
                    subdivisionEntry!,
                    "Subdivision limit",
                    positive: false,
                    maximum: GIProbesBakeSettings.MaximumSubdivisionLevel),
                ReadFloat(spacingEntry!, "Min probe spacing", positive: true),
                ReadFloat(normalBiasEntry!, "Normal bias", positive: false),
                ReadFloat(viewBiasEntry!, "View bias", positive: false),
                ReadFloat(maxDistanceEntry!, "Max ray distance", positive: true),
                includeSky!.IsChecked,
                includeEmissive!.IsChecked,
                includeDirect!.IsChecked);
            var request = new GIProbesBakeRequest(
                worldService.CurrentWorldAsset.FileId,
                outputPath,
                stateName,
                settings,
                LayoutSource: reuseLayout ? layoutSource.Value : null,
                FallbackEnvironment: ReadVector(
                    environmentEntry!,
                    "Fallback environment"),
                Overwrite: overwrite!.IsChecked,
                ThreadCount: ReadUInt(
                    threadCountEntry!,
                    "Bake threads",
                    positive: true,
                    maximum: GIProbesBakeRequest.MaximumThreadCount));

            bakeUiOperationActive = true;
            bakeLaunchPending = true;
            handledSuccess = false;
            SetBakeRunning(true);
            if (cancelBakeButton is not null)
                cancelBakeButton.IsEnabled = false;
            launchStage = "Saving the level before baking";
            SetBakeStatus("Saving the level before baking...", false);
            var save = await worldService.SaveCurrentWorldAsync(confirmExisting: false);
            if (save.Outcome != SceneSaveOutcome.Saved)
                throw new InvalidOperationException(
                    save.Error ?? "The level could not be saved before baking.");
            if (!IsCurrentWorld() || worldService.CurrentWorldAsset?.FileId is null)
                throw new InvalidOperationException(
                    "The saved level is no longer the active world asset.");

            launchStage = "Starting the native bake controller";
            if (!await engineService.StartGIProbesBakeAsync(request))
                throw new InvalidOperationException("The native bake controller rejected the request.");
            bakeLaunchPending = false;
            await PollStatusAsync();
        }
        catch (Exception exception)
        {
            bakeUiOperationActive = false;
            bakeLaunchPending = false;
            bakeStatusGate.PreserveTerminalStatus();
            SetBakeStatus($"{launchStage} failed: {exception.Message}", true);
            SetBakeRunning(false);
        }
    }

    async Task CancelBakeAsync()
    {
        try
        {
            await engineService.CancelGIProbesBakeAsync();
        }
        catch (Exception exception)
        {
            bakeStatusGate.PreserveTerminalStatus();
            SetBakeStatus(exception.Message, true);
        }
    }

    async void PollStatus(object? sender, EventArgs arguments)
    {
        await PollStatusAsync();
    }

    async Task PollStatusAsync()
    {
        SynchronizeWorld(refreshBindings: false);
        if (polling)
            return;
        polling = true;
        try
        {
            await RefreshRuntimeStateAsync();
            if (!IsCurrentWorld())
                return;
            var status = await engineService.GetGIProbesBakeStatusAsync();
            if (status is null)
                return;
            if (bakeLaunchPending)
                return;
            if (!bakeStatusGate.ShouldApplyPolledStatus(status.IsRunning))
                return;
            var shouldComplete =
                bakeUiOperationActive &&
                status.State == GIProbesBakeLifecycleState.Succeeded &&
                !handledSuccess;
            SetBakeRunning(status.IsRunning || shouldComplete);
            if (bakeProgress is not null)
                bakeProgress.Progress = Math.Clamp(status.Progress, 0.0f, 1.0f);
            var counts = status.TotalProbes > 0
                ? $" {status.CompletedProbes}/{status.TotalProbes} probes"
                : string.Empty;
            SetBakeStatus(
                shouldComplete
                    ? $"Finalizing: validating and activating the baked state{counts}..."
                    : $"{status.State}: {status.Stage}{counts} ({status.ElapsedSeconds:0.0}s)" +
                        (string.IsNullOrWhiteSpace(status.Diagnostic)
                            ? string.Empty
                            : $"{Environment.NewLine}{status.Diagnostic}"),
                status.State == GIProbesBakeLifecycleState.Failed);
            if (shouldComplete)
            {
                handledSuccess = true;
                try
                {
                    await CompleteBakeAsync(status);
                }
                finally
                {
                    bakeUiOperationActive = false;
                    bakeStatusGate.PreserveTerminalStatus();
                    SetBakeRunning(false);
                }
            }
            else if (bakeUiOperationActive && !status.IsRunning)
            {
                bakeUiOperationActive = false;
                bakeStatusGate.PreserveTerminalStatus();
            }
        }
        catch (Exception exception)
        {
            bakeStatusGate.PreserveTerminalStatus();
            SetBakeStatus(exception.Message, true);
        }
        finally
        {
            polling = false;
        }
    }

    async Task CompleteBakeAsync(GIProbesBakeStatus status)
    {
        if (!await engineService.RequestAssetReloadAsync())
            throw new InvalidOperationException("The bake succeeded, but the asset registry reload failed.");
        await assetsService.RefreshAsync();
        var physicalPath = Path.GetFullPath(Path.Combine(
            assetsService.CurrentProjectRootPath,
            status.OutputVirtualPath));
        var outputDirectory = Path.GetDirectoryName(physicalPath) ??
            throw new InvalidOperationException(
                "The baked .probes output directory could not be resolved.");
        await assetsService.ResolveFolderAsync(outputDirectory);
        var asset = assetsService.Files
            .OfType<GIProbesFile>()
            .FirstOrDefault(candidate =>
                candidate.Asset is not null &&
                ProjectContentPathPolicy.IsSamePath(
                    candidate.Asset.FullName,
                    physicalPath));
        if (asset is null)
            throw new InvalidOperationException("The baked .probes file was not discovered as an asset.");

        var metadata = GIProbesBinaryMetadata.Read(asset.Asset);
        if (metadata.LayoutHash != status.LayoutHash ||
            metadata.TransportHash != status.TransportHash ||
            metadata.LightingHash != status.LightingHash)
        {
            throw new InvalidOperationException(
                "The saved .probes identity does not match the completed native bake.");
        }

        var name = stateNameEntry!.Text?.Trim() ?? string.Empty;
        var previousBindings = CaptureBindingDrafts();
        var previousMode = selectedGlobalIlluminationMode;
        var currentWorldWasDirty = worldService.CurrentWorldAsset?.IsDirty ?? false;
        var inspectedWorldWasDirty = worldFile?.IsDirty ?? false;
        var bindingsMutated = false;
        try
        {
            var existing = bindings.FirstOrDefault(binding =>
                string.Equals(binding.Name, name, StringComparison.Ordinal));
            existing ??= new BindingDraft { Name = name };
            if (!bindings.Contains(existing))
                bindings.Add(existing);
            bindingsMutated = true;
            existing.Asset = asset.FileId;
            existing.Mode = bakedMode!.SelectedItem is GlobalIlluminationCompositionMode mode
                ? mode
                : GlobalIlluminationCompositionMode.Blend;
            var targetWeight = ReadFloat(
                bakedWeight!,
                "Binding weight",
                positive: false);
            existing.InitialWeightText = targetWeight.ToString(
                CultureInfo.InvariantCulture);
            existing.Preload = bakedPreload!.IsChecked;

            var targetIdentity = ToCompositionIdentity(metadata);
            var activeStates = new List<GIProbesBindingCompositionState>();
            foreach (var binding in bindings)
            {
                if (string.Equals(binding.Name, name, StringComparison.Ordinal))
                    continue;
                if (!GlobalIlluminationBindingInputPolicy.TryParseInitialWeight(
                        binding.InitialWeightText,
                        out var weight))
                {
                    throw new InvalidOperationException(
                        $"Probe state '{binding.Name}' has an invalid weight.");
                }
                if (weight == 0.0f)
                    continue;
                var boundAsset = ResolveGIProbesAsset(binding);
                var boundMetadata = GIProbesBinaryMetadata.Read(boundAsset.Asset);
                activeStates.Add(new GIProbesBindingCompositionState(
                    binding.Name,
                    weight,
                    ToCompositionIdentity(boundMetadata)));
            }

            var deactivated = GIProbesBakeBindingPolicy.FindIncompatibleActiveStates(
                name,
                targetWeight,
                targetIdentity,
                activeStates);
            foreach (var binding in bindings.Where(binding =>
                deactivated.Contains(binding.Name, StringComparer.Ordinal)))
            {
                binding.InitialWeightText = "0";
            }
            RebuildBindingRows();

            var baseline = await engineService.GetGlobalIlluminationStateAsync() ??
                throw new InvalidOperationException(
                    "The bake succeeded, but its runtime activation baseline is unavailable.");
            if (!await ApplyBindingsCoreAsync(refreshRuntime: false))
            {
                throw new InvalidOperationException(
                    "The bake succeeded, but the level binding was rejected.");
            }

            var activationRequired =
                targetWeight > 0.0f &&
                selectedGlobalIlluminationMode == GlobalIlluminationRuntimeMode.Baked &&
                baseline.Enabled;
            if (activationRequired)
            {
                await WaitForBakedStateActivationAsync(
                    name,
                    asset.FileId.Value,
                    baseline);
            }

            var save = await worldService.SaveCurrentWorldAsync(confirmExisting: false);
            if (save.Outcome != SceneSaveOutcome.Saved)
            {
                throw new InvalidOperationException(
                    save.Error ?? "The baked binding could not be saved to the level.");
            }
            var deactivatedMessage = deactivated.Count == 0
                ? string.Empty
                : $" Deactivated incompatible states: {string.Join(", ", deactivated)}.";
            var activationMessage = activationRequired
                ? " Runtime composition verified."
                : " Runtime activation is not required by the current GI mode or weight.";
            SetBakeStatus(
                $"Succeeded: {status.ProbeCount} probes in {status.BrickCount} bricks. Bound '{name}' and saved the level.{deactivatedMessage}{activationMessage}",
                false);
        }
        catch (Exception exception)
        {
            if (!bindingsMutated)
                throw;
            var rollbackDiagnostic = await RestoreBindingsAfterFailedBakeAsync(
                previousBindings,
                previousMode,
                currentWorldWasDirty,
                inspectedWorldWasDirty);
            throw new InvalidOperationException(
                string.IsNullOrEmpty(rollbackDiagnostic)
                    ? $"{exception.Message} Previous level bindings were restored."
                    : $"{exception.Message} Binding rollback also failed: {rollbackDiagnostic}",
                exception);
        }
    }

    static GIProbesCompositionIdentity ToCompositionIdentity(
        GIProbesBinaryMetadata metadata) => new(
            metadata.LayoutHash,
            metadata.RepresentationHash,
            metadata.TransportHash);

    GIProbesFile ResolveGIProbesAsset(BindingDraft binding)
    {
        var asset = assetsService.Files
            .OfType<GIProbesFile>()
            .FirstOrDefault(candidate => string.Equals(
                candidate.FileId?.Value,
                binding.Asset?.Value,
                StringComparison.Ordinal));
        return asset ?? throw new InvalidOperationException(
            $"Probe state '{binding.Name}' does not resolve to a readable .probes asset.");
    }

    List<BindingDraftSnapshot> CaptureBindingDrafts() => bindings.Select(binding =>
        new BindingDraftSnapshot(
            binding.Name,
            new FileId(binding.Asset?.Value ?? string.Empty),
            binding.Mode,
            binding.InitialWeightText,
            binding.Preload)).ToList();

    void RestoreBindingDrafts(IReadOnlyCollection<BindingDraftSnapshot> snapshots)
    {
        bindings.Clear();
        bindings.AddRange(snapshots.Select(snapshot => new BindingDraft
        {
            Name = snapshot.Name,
            Asset = new FileId(snapshot.Asset?.Value ?? string.Empty),
            Mode = snapshot.Mode,
            InitialWeightText = snapshot.InitialWeightText,
            Preload = snapshot.Preload
        }));
        RebuildBindingRows();
    }

    async Task<string> RestoreBindingsAfterFailedBakeAsync(
        IReadOnlyCollection<BindingDraftSnapshot> previousBindings,
        GlobalIlluminationRuntimeMode previousMode,
        bool currentWorldWasDirty,
        bool inspectedWorldWasDirty)
    {
        try
        {
            selectedGlobalIlluminationMode = previousMode;
            RestoreBindingDrafts(previousBindings);
            if (!await engineService.SetGISettingsAsync(
                    previousMode,
                    BuildRuntimeSettingsDescriptor(),
                    BuildBindingDescriptors()))
            {
                return "the Global Illumination ECS rejected the previous bindings";
            }
            if (worldService.CurrentWorldAsset is not null)
                worldService.CurrentWorldAsset.IsDirty = currentWorldWasDirty;
            if (worldFile is not null)
                worldFile.IsDirty = inspectedWorldWasDirty;
            await RefreshRuntimeStateAsync(
                "Reading restored runtime state",
                "Restored");
            return string.Empty;
        }
        catch (Exception exception)
        {
            return exception.Message;
        }
    }

    async Task WaitForBakedStateActivationAsync(
        string targetName,
        string targetAssetFileId,
        GlobalIlluminationRuntimeState baseline)
    {
        SetRuntimeStatus(
            $"Waiting for baked state '{targetName}' to become active...",
            false);
        const int ActivationTimeoutSeconds = 30;
        var deadline = DateTime.UtcNow +
            TimeSpan.FromSeconds(ActivationTimeoutSeconds);
        GIProbesBakeActivationAssessment? lastAssessment = null;
        while (DateTime.UtcNow < deadline)
        {
            var current = await engineService.GetGlobalIlluminationStateAsync();
            lastAssessment = GIProbesBakeBindingPolicy.AssessActivation(
                targetName,
                targetAssetFileId,
                activationRequired: true,
                baseline,
                current);
            if (lastAssessment.State == GIProbesBakeActivationState.Succeeded)
            {
                await RefreshRuntimeStateAsync(
                    "Reading verified runtime state",
                    "Baked state active");
                return;
            }
            if (lastAssessment.State == GIProbesBakeActivationState.Rejected)
                throw new InvalidOperationException(lastAssessment.Diagnostic);
            await Task.Delay(100);
        }
        throw new TimeoutException(
            $"The baked state was not activated within {ActivationTimeoutSeconds} seconds. {lastAssessment?.Diagnostic}".Trim());
    }

    async Task RefreshRuntimeStateAsync(
        string pendingStatus = "",
        string successPrefix = "")
    {
        try
        {
            if (!string.IsNullOrWhiteSpace(pendingStatus))
                SetRuntimeStatus($"{pendingStatus}...", false);
            var state = await engineService.GetGlobalIlluminationStateAsync();
            if (state is null)
            {
                SetRuntimeStatus(
                    "Global Illumination runtime state is unavailable.",
                    true);
                return;
            }
            lastRuntimeGIState = state.RuntimeState;
            if (runtimeProgress is not null)
            {
                runtimeProgress.Progress = Math.Clamp(
                    state.RuntimeState.Coverage,
                    0.0f,
                    1.0f);
            }
            if (runtimeRefinementProgress is not null)
            {
                runtimeRefinementProgress.Progress = Math.Clamp(
                    state.RuntimeState.Refinement,
                    0.0f,
                    1.0f);
            }
            UpdateRuntimeBudgetPicker(state.RuntimeState.PreviewBudget);
            if (runtimePreviewButton is not null)
            {
                runtimePreviewButton.Text = state.RuntimeState.PreviewEnabled
                    ? "Disable Preview"
                    : "Enable Preview";
            }
            if (runtimePauseButton is not null)
            {
                runtimePauseButton.Text = state.RuntimeState.Paused
                    ? "Resume"
                    : "Pause";
                runtimePauseButton.IsEnabled = state.RuntimeState.Enabled;
            }
            var probes = state.Probes.Count == 0
                ? "no states"
                : string.Join(
                    ", ",
                    state.Probes.Select(probe =>
                        $"{probe.Name}={probe.Weight:0.###} {probe.Mode}/{probe.Residency}"));
            var prefix = string.IsNullOrWhiteSpace(successPrefix)
                ? string.Empty
                : $"{successPrefix} {DateTime.Now:HH:mm:ss}: ";
            var runtimeDetails = state.Mode ==
                GlobalIlluminationRuntimeMode.Runtime
                ? $" Runtime {state.RuntimeState.Lifecycle}; " +
                    $"{state.RuntimeState.ReadyProbeCount}/{state.RuntimeState.ActiveProbeCount} ready; " +
                    $"coverage {state.RuntimeState.Coverage:P0}, " +
                    $"refinement {state.RuntimeState.Refinement:P0}; " +
                    $"{state.RuntimeState.WorkerCount} worker(s), " +
                    $"{state.RuntimeState.PreviewBudget} budget. " +
                    state.RuntimeState.Diagnostic
                : string.Empty;
            SetRuntimeStatus(
                $"{prefix}{state.Mode}; profile GI {(state.Enabled ? "enabled" : "disabled")}; budget {state.MaxProbeStatesPerSnapshot}; {probes}. {state.Diagnostic}{runtimeDetails}",
                state.Probes.Any(probe =>
                    probe.Residency == GlobalIlluminationResidency.Failed) ||
                    state.RuntimeState.Lifecycle ==
                        RuntimeGIProbesLifecycleState.Failed);
        }
        catch (Exception exception)
        {
            SetRuntimeStatus(exception.Message, true);
        }
    }

    async Task ExecuteRuntimeCommandAsync(
        Func<Task<bool>> command,
        string rejectionDiagnostic)
    {
        try
        {
            if (!await command())
            {
                SetRuntimeStatus(rejectionDiagnostic, true);
                return;
            }
            await RefreshRuntimeStateAsync();
        }
        catch (Exception exception)
        {
            SetRuntimeStatus(exception.Message, true);
        }
    }

    async Task RefreshRuntimeEditorControlsAsync()
    {
        try
        {
            var snapshot = await graphicsSettingsService.EnsureLoadedAsync();
            Dispatcher.Dispatch(() => UpdateRuntimeEditorControls(
                snapshot.Editor.Graphics));
            var renderMode = await engineService.GetEditorRenderModeAsync();
            if (renderMode.HasValue)
            {
                Dispatcher.Dispatch(() =>
                    UpdateRuntimeDebugViewPicker(renderMode.Value));
            }
        }
        catch (Exception exception)
        {
            SetRuntimeStatus(exception.Message, true);
        }
    }

    void OnGraphicsSettingsChanged(
        object? sender,
        GraphicsSettingsSnapshot snapshot) =>
        Dispatcher.Dispatch(() =>
            UpdateRuntimeEditorControls(snapshot.Editor.Graphics));

    void OnEditorRenderModeChanged(SceneViewRenderMode mode) =>
        Dispatcher.Dispatch(() => UpdateRuntimeDebugViewPicker(mode));

    void UpdateRuntimeEditorControls(EditorGraphicsSettings settings)
    {
        updatingRuntimeControls = true;
        try
        {
            if (runtimeBudget is not null)
            {
                runtimeBudget.SelectedItem = RuntimeBudgets.Single(option =>
                    option.Value == settings.RuntimeGIProbesBudget);
            }
            if (runtimeDebugView is not null)
            {
                runtimeDebugView.SelectedItem = RuntimeDebugViews.Single(option =>
                    option.Value == settings.RuntimeGIProbesDebugView);
            }
        }
        finally
        {
            updatingRuntimeControls = false;
        }
    }

    void UpdateRuntimeBudgetPicker(RuntimeGIProbesEditorBudget budget)
    {
        if (runtimeBudget is null)
            return;
        updatingRuntimeControls = true;
        try
        {
            runtimeBudget.SelectedItem = RuntimeBudgets.Single(option =>
                option.Value == budget);
        }
        finally
        {
            updatingRuntimeControls = false;
        }
    }

    void UpdateRuntimeDebugViewPicker(SceneViewRenderMode mode)
    {
        if (runtimeDebugView is null)
            return;
        var option = RuntimeDebugViews.FirstOrDefault(candidate =>
            candidate.RenderMode == mode);
        if (option is null)
            return;
        updatingRuntimeControls = true;
        try
        {
            runtimeDebugView.SelectedItem = option;
        }
        finally
        {
            updatingRuntimeControls = false;
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

    bool StateOutputExists(string state)
    {
        if (!GIProbesBakeOutputPolicy.TryResolveWriteTarget(
                assetsService.CurrentProjectRootPath,
                DefaultOutputPath(state),
                overwrite: true,
                out var physicalPath,
                out _))
        {
            return false;
        }

        return File.Exists(physicalPath) || File.Exists(physicalPath + ".asset");
    }

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
        {
            entry.TextChanged += (_, args) =>
                changed(args.NewTextValue ?? string.Empty);
        }
        return entry;
    }

    static Entry UIntEntry(uint value) => new()
    {
        Text = value.ToString(CultureInfo.InvariantCulture),
        Keyboard = Keyboard.Numeric,
        ReturnType = ReturnType.Done
    };

    static Entry FloatEntry(float value) => new()
    {
        Text = value.ToString(CultureInfo.InvariantCulture),
        Keyboard = Keyboard.Numeric,
        ReturnType = ReturnType.Done
    };

    static Entry FloatTextEntry(string value, Action<string> changed)
    {
        var entry = new Entry
        {
            Text = value,
            Keyboard = Keyboard.Numeric,
            ReturnType = ReturnType.Done
        };
        entry.TextChanged += (_, args) =>
            changed(args.NewTextValue ?? string.Empty);
        return entry;
    }

    static CheckBox BoolEditor(bool value, Action<bool> changed)
    {
        var checkBox = new CheckBox { IsChecked = value };
        checkBox.CheckedChanged += (_, args) => changed(args.Value);
        return checkBox;
    }

    static Button ActionButton(
        string text,
        Func<Task> action,
        bool restoreEnabledAfterAction = true)
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
                if (restoreEnabledAfterAction && button.Handler is not null)
                    button.IsEnabled = true;
            }
        };
        return button;
    }

    static Button ActionButton(
        string text,
        Func<Task<bool>> action,
        bool restoreEnabledAfterAction = true) =>
        ActionButton(
            text,
            async () => { _ = await action(); },
            restoreEnabledAfterAction);
}
