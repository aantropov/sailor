#nullable enable

using CommunityToolkit.Maui.Storage;
using SailorEditor.Workspace;

namespace SailorEditor.Views;

public sealed class GameBuildPage : ContentPage
{
    readonly string _workspace;
    readonly GameBuildService _service;
    GameBuildSettings _settings;
    readonly Picker _platform = new() { Title = "Platform" };
    readonly Picker _configuration = new() { Title = "Configuration", ItemsSource = new[] { "Debug", "Release" } };
    readonly Picker _assets = new() { Title = "Assets" };
    readonly Picker _compression = new() { Title = "Compression" };
    readonly CheckBox _initialCache = new();
    readonly Entry _output = new() { Placeholder = "New output directory outside the project" };
    readonly Entry _engine = new() { Placeholder = "Engine source directory or installed SDK" };
    readonly Entry _engineBuild = new() { Placeholder = "Automatic: separate directory per configuration" };
    readonly Button _build = new() { Text = "Build Game" };
    readonly Button _save = new() { Text = "Save Settings" };
    readonly Button _cancel = new() { Text = "Cancel Build", IsEnabled = false };
    readonly Button _browse = new() { Text = "Choose Output Parent…" };
    readonly Label _status = new() { Text = "Ready" };
    readonly Microsoft.Maui.Controls.Editor _log = new() { IsReadOnly = true, HeightRequest = 240, FontSize = 11 };
    GameBuildPlatform? _selectedPlatform;
    CancellationTokenSource? _cancellation;

    public GameBuildPage(string workspace, string engine, GameBuildService service)
    {
        _workspace = workspace;
        _service = service;
        _settings = GameBuildSettingsStore.Load(workspace);
        Title = "Build Game";
        _platform.ItemsSource = Enum.GetValues<GameBuildPlatform>();
        _assets.ItemsSource = Enum.GetValues<GameAssetPackaging>();
        _compression.ItemsSource = Enum.GetValues<GameArchiveCompression>();
        _engine.Text = engine;
        _configuration.SelectedItem = _settings.Configuration;
        _platform.SelectedIndexChanged += (_, _) => SelectPlatform();
        _platform.SelectedItem = GameBuildSettings.HostPlatform;
        _build.Clicked += async (_, _) => await BuildAsync();
        _save.Clicked += (_, _) => SaveSettings();
        _cancel.Clicked += (_, _) => Cancel();
        _browse.Clicked += async (_, _) =>
        {
            try
            {
                var picked = await FolderPicker.Default.PickAsync(CancellationToken.None);
                if (picked.IsSuccessful)
                {
                    _output.Text = Path.Combine(picked.Folder.Path,
                        Path.GetFileName(_workspace) + "-" + _platform.SelectedItem + "-" + _configuration.SelectedItem);
                }
            }
            catch (Exception error)
            {
                _status.Text = error.Message;
            }
        };
        var content = new VerticalStackLayout { Padding = 20, Spacing = 10 };
        content.Add(new Label { Text = "Build Game", FontSize = 22, FontAttributes = FontAttributes.Bold });
        content.Add(new Label { Text = workspace, FontSize = 12, LineBreakMode = LineBreakMode.MiddleTruncation });
        AddField("Target platform", _platform);
        AddField("Configuration", _configuration);
        AddField("Asset layout", _assets);
        AddField("ZIP compression (also used for initial cache)", _compression);
        content.Add(new HorizontalStackLayout { Spacing = 8, Children =
            { _initialCache, new Label { Text = "Include initial shader and model cache", VerticalOptions = LayoutOptions.Center } } });
        AddField("Output directory", _output);
        content.Add(_browse);
        AddField("Engine source or SDK", _engine);
        AddField("Engine build directory (optional)", _engineBuild);
        content.Add(new HorizontalStackLayout { Spacing = 10, Children = { _build, _save, _cancel } });
        content.Add(_status);
        content.Add(_log);
        Content = new ScrollView { Content = content };

        void AddField(string label, View view)
        {
            content.Add(new Label { Text = label, FontSize = 12, FontAttributes = FontAttributes.Bold });
            content.Add(view);
        }
    }

    public void Cancel() => _cancellation?.Cancel();

    void CapturePlatform()
    {
        if (_selectedPlatform is { } platform)
        {
            _settings.Platforms[platform] = new GamePlatformBuildSettings
            {
                Assets = _assets.SelectedItem is GameAssetPackaging assets ? assets : GameAssetPackaging.Folders,
                Compression = _compression.SelectedItem is GameArchiveCompression compression ? compression : GameArchiveCompression.Balanced,
                IncludeInitialCache = _initialCache.IsChecked,
                OutputDirectory = _output.Text ?? string.Empty
            };
        }
    }

    void SelectPlatform()
    {
        CapturePlatform();
        if (_platform.SelectedItem is not GameBuildPlatform platform)
        {
            return;
        }
        _selectedPlatform = platform;
        var settings = _settings.Platforms[platform];
        _assets.SelectedItem = settings.Assets;
        _compression.SelectedItem = settings.Compression;
        _initialCache.IsChecked = settings.IncludeInitialCache;
        _output.Text = settings.OutputDirectory;
        if (string.IsNullOrWhiteSpace(_output.Text))
        {
            _output.Text = Path.Combine(Path.GetDirectoryName(_workspace)!, Path.GetFileName(_workspace) + "-Builds",
                platform.ToString(), (_configuration.SelectedItem as string ?? "Release") + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        }
        _build.IsEnabled = platform == GameBuildSettings.HostPlatform;
        _status.Text = _build.IsEnabled ? "Ready" : $"Save these options, then build on a native {platform} host.";
    }

    bool SaveSettings()
    {
        try
        {
            CapturePlatform();
            _settings = _settings with { Configuration = _configuration.SelectedItem as string ?? "Release" };
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

    async Task BuildAsync()
    {
        if (_cancellation is not null || !SaveSettings() || _selectedPlatform is not { } platform)
        {
            return;
        }
        var options = _settings.Platforms[platform];
        var request = new GameBuildRequest(_workspace, _engine.Text ?? string.Empty, options.OutputDirectory,
            _settings.Configuration, platform, options)
        {
            EngineBuildDirectory = string.IsNullOrWhiteSpace(_engineBuild.Text) ? null : _engineBuild.Text
        };
        using var cancellation = new CancellationTokenSource();
        _cancellation = cancellation;
        SetBuilding(true);
        _log.Text = string.Empty;
        var progress = new Progress<string>(text =>
        {
            var log = (_log.Text ?? string.Empty) + text + Environment.NewLine;
            _log.Text = log.Length > 32000 ? log[^32000..] : log;
        });
        try
        {
            _status.Text = "Building…";
            var result = await _service.BuildAsync(request, progress, cancellation.Token);
            _status.Text = result.Succeeded ? "Build complete: " + result.OutputDirectory : "Build failed: " + result.Error;
        }
        catch (OperationCanceledException)
        {
            _status.Text = "Build cancelled.";
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
        foreach (var view in new VisualElement[] { _platform, _configuration, _assets, _compression, _initialCache,
            _output, _engine, _engineBuild, _build, _save, _browse })
        {
            view.IsEnabled = !building;
        }
        _cancel.IsEnabled = building;
    }
}
