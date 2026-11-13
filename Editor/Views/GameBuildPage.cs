#nullable enable

using CommunityToolkit.Maui.Storage;
using SailorEditor.Workspace;

namespace SailorEditor.Views;

public sealed class GameBuildPage : ContentPage
{
    readonly string _workspace;
    readonly string _contentRoot;
    readonly GameBuildService _service;
    GameBuildSettings _settings;
    readonly Picker _platform = InputPicker("BuildPlatform");
    readonly Picker _scene = InputPicker("StartupScene");
    readonly Picker _assets = InputPicker("AssetPackaging");
    readonly Picker _compression = InputPicker("ArchiveCompression");
    readonly RadioButton _debug = new() { Content = "Debug", AutomationId = "BuildDebug" };
    readonly RadioButton _release = new() { Content = "Release", AutomationId = "BuildRelease" };
    readonly CheckBox _initialCache = new() { AutomationId = "IncludeInitialCache" };
    readonly Entry _output = InputEntry("BuildOutput", "New output directory");
    readonly Entry _engine = InputEntry("BuildEngine", "Engine source directory or installed SDK");
    readonly Entry _engineBuild = InputEntry("EngineBuildDirectory", "Automatic: reuse a matching build or create one");
    readonly Entry _toolchain = InputEntry("BuildToolchain", "CMake toolchain file (optional for native builds)");
    readonly Button _build = new() { Text = "Build", AutomationId = "BuildOnly" };
    readonly Button _buildRun = new() { Text = "Build & Run", AutomationId = "BuildAndRun" };
    readonly Button _save = new() { Text = "Save Settings" };
    readonly Button _cancel = new() { Text = "Cancel Build", IsEnabled = false };
    readonly Button _browse = new() { Text = "Browse…" };
    readonly Button _chooseToolchain = new() { Text = "Browse…" };
    readonly Button _check = new() { Text = "Check Toolchain" };
    readonly Button _advancedToggle = new() { Text = "Advanced Options ▸", HorizontalOptions = LayoutOptions.Start };
    readonly Label _availability = new() { FontSize = 12, LineBreakMode = LineBreakMode.WordWrap };
    readonly Label _status = new() { Text = "Ready", LineBreakMode = LineBreakMode.WordWrap, AutomationId = "BuildStatus" };
    readonly ActivityIndicator _activity = new() { IsVisible = false };
    readonly Microsoft.Maui.Controls.Editor _log = new()
    {
        IsReadOnly = true, HeightRequest = 180, FontSize = 11, IsVisible = false, AutomationId = "BuildLog"
    };
    readonly VerticalStackLayout _advanced = new() { Spacing = 10, IsVisible = false };
    readonly List<VisualElement> _inputs = [];
    GameBuildPlatform? _selectedPlatform;
    GameBuildCapability? _capability;
    CancellationTokenSource? _cancellation;
    CancellationTokenSource? _checking;
    Task? _operation;
    bool _initializing = true;

    string Configuration => _debug.IsChecked ? "Debug" : "Release";

    public GameBuildPage(string workspace, string engine, GameBuildService service,
        string? contentRoot = null, string? currentScene = null)
    {
        _workspace = workspace;
        _contentRoot = contentRoot ?? Path.Combine(workspace, "Content");
        _service = service;
        _settings = GameBuildSettingsStore.Load(workspace);
        Title = "Build Game";
        Padding = 20;
        _platform.ItemsSource = Enum.GetNames<GameBuildPlatform>();
        _assets.ItemsSource = new[] { "Folders", "Bundles (.bundle)" };
        _compression.ItemsSource = Enum.GetNames<GameArchiveCompression>();
        _engine.Text = engine;
        var group = "BuildConfiguration-" + Guid.NewGuid().ToString("N");
        _debug.GroupName = group;
        _release.GroupName = group;
        _debug.IsChecked = _settings.Configuration == "Debug";
        _release.IsChecked = !_debug.IsChecked;
        var scenes = Directory.Exists(_contentRoot) ? Directory.EnumerateFiles(_contentRoot, "*.world",
            new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint })
            .Select(path => Path.GetRelativePath(_contentRoot, path).Replace('\\', '/'))
            .Order(StringComparer.OrdinalIgnoreCase).ToList() : [];
        _scene.ItemsSource = scenes;
        _scene.SelectedItem = scenes.Contains(_settings.StartupScene) ? _settings.StartupScene :
            currentScene is not null && scenes.Contains(currentScene) ? currentScene : scenes.FirstOrDefault();
        _scene.Title = scenes.Count == 0 ? "No scenes — creates an empty world" : "Choose a scene";

        var form = new VerticalStackLayout { Spacing = 14 };
        var target = new Grid { ColumnDefinitions = new ColumnDefinitionCollection(new(GridLength.Star), new(GridLength.Star)), ColumnSpacing = 18 };
        target.Add(Field("Target Platform", _platform), 0);
        target.Add(Field("Configuration", new HorizontalStackLayout { Spacing = 12, Children = { _debug, _release } }), 1);
        form.Add(target);
        form.Add(_availability);
        form.Add(Field("Startup Scene", _scene));
        var output = new Grid { ColumnDefinitions = new ColumnDefinitionCollection(new(GridLength.Star), new(GridLength.Auto)), ColumnSpacing = 8 };
        output.Add(_output, 0);
        output.Add(_browse, 1);
        form.Add(Field("Output Directory", output));
        var packaging = new Grid { ColumnDefinitions = new ColumnDefinitionCollection(new(GridLength.Star), new(GridLength.Star)), ColumnSpacing = 18 };
        packaging.Add(Field("Asset Layout", _assets), 0);
        packaging.Add(Field("Compression", _compression), 1);
        form.Add(packaging);
        form.Add(new HorizontalStackLayout { Spacing = 6, Children =
            { _initialCache, new Label { Text = "Include initial shader and model cache", VerticalOptions = LayoutOptions.Center, FontSize = 13 } } });
        form.Add(_advancedToggle);
        _advanced.Add(Field("Engine Source or SDK", _engine));
        _advanced.Add(Field("Engine Build Directory", _engineBuild));
        var toolchain = new Grid { ColumnDefinitions = new ColumnDefinitionCollection(new(GridLength.Star), new(GridLength.Auto)), ColumnSpacing = 8 };
        toolchain.Add(_toolchain, 0);
        toolchain.Add(_chooseToolchain, 1);
        _advanced.Add(Field("CMake Toolchain", toolchain));
        _advanced.Add(_check);
        form.Add(_advanced);
        form.Add(_log);

        var heading = new VerticalStackLayout { Spacing = 4, Margin = new Thickness(0, 0, 0, 16) };
        heading.Add(new Label { Text = "Build Game", FontSize = 24, FontAttributes = FontAttributes.Bold });
        heading.Add(new Label { Text = workspace, FontSize = 12, LineBreakMode = LineBreakMode.MiddleTruncation });
        var actions = new Grid { ColumnDefinitions = new ColumnDefinitionCollection(new(GridLength.Star), new(GridLength.Star)), ColumnSpacing = 12 };
        actions.Add(_build, 0);
        actions.Add(_buildRun, 1);
        var secondary = new HorizontalStackLayout { Spacing = 12, Children = { _save, _cancel } };
        var footer = new VerticalStackLayout { Spacing = 10, Margin = new Thickness(0, 14, 0, 0), Children = { _activity, _status, actions, secondary } };
        var root = new Grid { RowDefinitions = new RowDefinitionCollection(new(GridLength.Auto), new(GridLength.Star), new(GridLength.Auto)) };
        root.Add(heading, 0, 0);
        root.Add(new ScrollView { Content = form }, 0, 1);
        root.Add(footer, 0, 2);
        Content = root;
        _inputs.AddRange(new VisualElement[] { _platform, _scene, _debug, _release, _assets, _compression,
            _initialCache, _output, _engine, _engineBuild, _toolchain, _save, _browse, _chooseToolchain, _check });

        _build.Clicked += async (_, _) => await BuildAsync(run: false);
        _buildRun.Clicked += async (_, _) => await BuildAsync(run: true);
        _save.Clicked += (_, _) => SaveSettings();
        _cancel.Clicked += (_, _) => Cancel();
        _browse.Clicked += async (_, _) => await ChooseOutputAsync();
        _chooseToolchain.Clicked += async (_, _) => await ChooseToolchainAsync();
        _check.Clicked += async (_, _) => await CheckTargetAsync();
        _advancedToggle.Clicked += (_, _) => SetAdvanced(!_advanced.IsVisible);
        _toolchain.TextChanged += (_, _) => { _capability = null; UpdateActions(); };
        _toolchain.Unfocused += async (_, _) => await CheckTargetAsync();
        _platform.SelectedIndexChanged += async (_, _) => await SelectPlatformAsync();
        _debug.CheckedChanged += (_, args) =>
        {
            if (args.Value)
            {
                ConfigurationChanged();
            }
        };
        _release.CheckedChanged += (_, args) =>
        {
            if (args.Value)
            {
                ConfigurationChanged();
            }
        };
        _platform.SelectedIndex = (int)GameBuildSettings.HostPlatform;
        _initializing = false;
    }

    public void Cancel()
    {
        _cancellation?.Cancel();
        _checking?.Cancel();
    }

    public async Task CancelAsync()
    {
        Cancel();
        if (_operation is { } operation)
        {
            await operation;
        }
    }

    void CapturePlatform()
    {
        if (_selectedPlatform is { } platform)
        {
            _settings.Platforms[platform] = new GamePlatformBuildSettings
            {
                Assets = _assets.SelectedIndex == 1 ? GameAssetPackaging.Zip : GameAssetPackaging.Folders,
                Compression = Enum.TryParse<GameArchiveCompression>(_compression.SelectedItem as string, out var compression) ? compression : GameArchiveCompression.Balanced,
                IncludeInitialCache = _initialCache.IsChecked,
                OutputDirectory = _output.Text ?? string.Empty,
                ToolchainFile = _toolchain.Text ?? string.Empty,
                EngineBuildDirectory = _engineBuild.Text ?? string.Empty
            };
        }
    }

    async Task SelectPlatformAsync()
    {
        CapturePlatform();
        if (_platform.SelectedIndex < 0)
        {
            return;
        }
        var platform = (GameBuildPlatform)_platform.SelectedIndex;
        _selectedPlatform = platform;
        var settings = _settings.Platforms[platform];
        _assets.SelectedIndex = (int)settings.Assets;
        _compression.SelectedItem = settings.Compression.ToString();
        _initialCache.IsChecked = settings.IncludeInitialCache;
        _output.Text = settings.OutputDirectory;
        _toolchain.Text = settings.ToolchainFile;
        _engineBuild.Text = settings.EngineBuildDirectory;
        if (string.IsNullOrWhiteSpace(_output.Text) || Directory.Exists(_output.Text) || File.Exists(_output.Text))
        {
            _output.Text = NewOutput();
        }
        if (platform != GameBuildSettings.HostPlatform)
        {
            SetAdvanced(true);
        }
        await CheckTargetAsync();
    }

    void ConfigurationChanged()
    {
        if (!_initializing && _selectedPlatform is not null)
        {
            _output.Text = NewOutput();
        }
    }

    string NewOutput() => Path.Combine(Path.GetDirectoryName(_workspace)!, Path.GetFileName(_workspace) + "-Builds",
        (_selectedPlatform ?? GameBuildSettings.HostPlatform).ToString(), Configuration + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff"));

    async Task CheckTargetAsync()
    {
        _checking?.Cancel();
        using var checking = new CancellationTokenSource();
        _checking = checking;
        _capability = null;
        UpdateActions();
        _availability.Text = "Checking build tools…";
        try
        {
            var capability = await GameBuildEnvironment.CheckAsync(_workspace,
                _selectedPlatform ?? GameBuildSettings.HostPlatform, _toolchain.Text ?? string.Empty,
                new WorkspaceProcessRunner(), cancellationToken: checking.Token);
            if (!checking.IsCancellationRequested)
            {
                _capability = capability;
                _availability.Text = capability.Detail;
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception error)
        {
            _availability.Text = error.Message;
        }
        finally
        {
            if (ReferenceEquals(_checking, checking))
            {
                _checking = null;
            }
            UpdateActions();
        }
    }

    async Task ChooseOutputAsync()
    {
        try
        {
            var picked = await FolderPicker.Default.PickAsync(CancellationToken.None);
            if (picked.IsSuccessful)
            {
                _output.Text = Path.Combine(picked.Folder.Path, Path.GetFileName(_workspace) + "-" +
                    _platform.SelectedItem + "-" + Configuration + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff"));
            }
        }
        catch (Exception error)
        {
            _status.Text = error.Message;
        }
    }

    async Task ChooseToolchainAsync()
    {
        try
        {
            var file = await FilePicker.Default.PickAsync(new PickOptions { PickerTitle = "Choose a CMake toolchain" });
            if (file is not null)
            {
                _toolchain.Text = file.FullPath;
                await CheckTargetAsync();
            }
        }
        catch (Exception error)
        {
            _status.Text = error.Message;
        }
    }

    bool SaveSettings()
    {
        try
        {
            CapturePlatform();
            _settings = _settings with { Configuration = Configuration, StartupScene = _scene.SelectedItem as string ?? string.Empty };
            GameBuildSettingsStore.Save(_workspace, _settings);
            _status.Text = "Build settings saved.";
            return true;
        }
        catch (Exception error)
        {
            _status.Text = error.Message;
            return false;
        }
    }

    async Task BuildAsync(bool run)
    {
        if (_operation is not null || _capability?.CanBuild != true)
        {
            return;
        }
        _operation = BuildCoreAsync(run);
        try
        {
            await _operation;
        }
        finally
        {
            _operation = null;
        }
    }

    async Task BuildCoreAsync(bool run)
    {
        if (Directory.Exists(_output.Text) || File.Exists(_output.Text))
        {
            _output.Text = NewOutput();
        }
        if (!SaveSettings() || _selectedPlatform is not { } platform)
        {
            return;
        }
        var options = _settings.Platforms[platform];
        var request = new GameBuildRequest(_workspace, _engine.Text ?? string.Empty, options.OutputDirectory,
            Configuration, platform, options) { StartupScene = _settings.StartupScene };
        using var cancellation = new CancellationTokenSource();
        _cancellation = cancellation;
        SetBuilding(true);
        _log.Text = string.Empty;
        _log.IsVisible = true;
        IProgress<string> progress = new Progress<string>(text =>
        {
            var log = (_log.Text ?? string.Empty) + text + Environment.NewLine;
            _log.Text = log.Length > 24000 ? log[^24000..] : log;
        });
        try
        {
            _status.Text = "Building " + platform + " / " + Configuration + "…";
            var result = await _service.BuildAsync(request, progress, cancellation.Token);
            if (!result.Succeeded)
            {
                _status.Text = "Build failed. " + result.Error;
            }
            else if (run)
            {
                var pid = GameBuildService.Run(result);
                _status.Text = "Build complete. Game running (PID " + pid + ").\n" + result.OutputDirectory;
            }
            else
            {
                _status.Text = "Build complete: " + result.OutputDirectory;
            }
            if (result.LogFile is not null)
            {
                progress.Report("Full log: " + result.LogFile);
            }
        }
        catch (OperationCanceledException)
        {
            _status.Text = "Build cancelled. Existing builds were preserved.";
        }
        catch (Exception error)
        {
            _status.Text = "Build failed: " + error.Message;
        }
        finally
        {
            _cancellation = null;
            SetBuilding(false);
        }
    }

    void SetBuilding(bool building)
    {
        foreach (var view in _inputs)
        {
            view.IsEnabled = !building;
        }
        _activity.IsVisible = _activity.IsRunning = building;
        _cancel.IsEnabled = building;
        UpdateActions();
    }

    void UpdateActions()
    {
        _build.IsEnabled = _buildRun.IsEnabled = _cancellation is null && _capability?.CanBuild == true;
    }

    void SetAdvanced(bool visible)
    {
        _advanced.IsVisible = visible;
        _advancedToggle.Text = visible ? "Advanced Options ▾" : "Advanced Options ▸";
    }

    static VerticalStackLayout Field(string title, View view) => new()
    {
        Spacing = 5,
        Children = { new Label { Text = title, FontSize = 12, FontAttributes = FontAttributes.Bold }, view }
    };

    static Picker InputPicker(string id)
    {
        var picker = new Picker { AutomationId = id, MinimumHeightRequest = 40, FontSize = 14, HorizontalOptions = LayoutOptions.Fill };
        picker.SetAppThemeColor(Picker.TextColorProperty, Colors.Black, Colors.White);
        picker.SetAppThemeColor(BackgroundColorProperty, Color.FromArgb("#EEEEEE"), Color.FromArgb("#303030"));
        return picker;
    }

    static Entry InputEntry(string id, string placeholder)
    {
        var entry = new Entry { AutomationId = id, Placeholder = placeholder, MinimumHeightRequest = 40, FontSize = 13 };
        entry.SetAppThemeColor(Entry.TextColorProperty, Colors.Black, Colors.White);
        entry.SetAppThemeColor(BackgroundColorProperty, Color.FromArgb("#EEEEEE"), Color.FromArgb("#303030"));
        return entry;
    }
}
