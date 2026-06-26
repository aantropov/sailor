#if MACCATALYST
using Foundation;
using System.Runtime.InteropServices;
using UniformTypeIdentifiers;
using UIKit;
#endif
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class WorkspaceUiService
{
    readonly WorkspaceLifecycleService _workspaceLifecycle;
    readonly EngineService _engineService;
    static readonly FilePickerFileType WorkspaceManifestFileType = new(new Dictionary<DevicePlatform, IEnumerable<string>>
    {
        { DevicePlatform.WinUI, [".sailor"] },
        { DevicePlatform.MacCatalyst, ["public.data"] },
        { DevicePlatform.iOS, ["public.data"] },
        { DevicePlatform.Android, ["*/*"] }
    });

    public WorkspaceUiService(WorkspaceLifecycleService workspaceLifecycle, EngineService engineService)
    {
        _workspaceLifecycle = workspaceLifecycle;
        _engineService = engineService;
    }

    public WorkspaceUiProjection Projection { get; private set; } = WorkspaceUiProjection.Empty;

    public event EventHandler? ProjectionChanged;

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        await RefreshAsync(cancellationToken);
    }

    public async Task RefreshAsync(CancellationToken cancellationToken = default)
    {
        var recent = await _workspaceLifecycle.LoadRecentAsync(cancellationToken);
        SetProjection(WorkspaceUiProjectionBuilder.Build(_workspaceLifecycle.Current, recent));
    }

    public async Task NewWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        if (page is null)
            return;

        var directory = await PickWorkspaceDirectoryAsync(page, cancellationToken);
        if (string.IsNullOrWhiteSpace(directory))
            return;

        var name = Path.GetFileName(Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (string.IsNullOrWhiteSpace(name))
            name = "SailorWorkspace";

        var result = await _workspaceLifecycle.CreateAsync(
            new WorkspaceCreateRequest(name, directory, _engineService.EngineWorkingDirectory),
            cancellationToken);

        await ApplyResultAsync("New Workspace", result, $"Created workspace '{name}'.", cancellationToken);
    }

    public async Task OpenWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        if (page is null)
            return;

        FileResult? picked;
        try
        {
            picked = await FilePicker.Default.PickAsync(new PickOptions
            {
                PickerTitle = "Load Sailor workspace",
                FileTypes = WorkspaceManifestFileType
            });
        }
        catch (Exception ex)
        {
            await page.DisplayAlert("Load Workspace", ex.Message, "OK");
            return;
        }

        if (picked is null || string.IsNullOrWhiteSpace(picked.FullPath))
            return;

        if (!string.Equals(Path.GetExtension(picked.FullPath), ".sailor", StringComparison.OrdinalIgnoreCase))
        {
            await page.DisplayAlert("Load Workspace", "Select a .sailor workspace manifest.", "OK");
            return;
        }

        await OpenWorkspaceAsync(picked.FullPath, cancellationToken);
    }

    public async Task OpenWorkspaceAsync(RecentWorkspaceEntry recentWorkspace, CancellationToken cancellationToken = default)
    {
        await OpenWorkspaceAsync(recentWorkspace.ManifestPath, cancellationToken);
    }

    public async Task OpenWorkspaceAsync(string manifestPath, CancellationToken cancellationToken = default)
    {
        var result = await _workspaceLifecycle.OpenAsync(manifestPath, cancellationToken);
        var name = result.Session?.Manifest.Name ?? Path.GetFileNameWithoutExtension(manifestPath);
        await ApplyResultAsync("Load Workspace", result, $"Loaded workspace '{name}'.", cancellationToken);
    }

    public async Task SaveWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        if (_workspaceLifecycle.Current is null)
        {
            if (page is not null)
                await page.DisplayAlert("Save Workspace", "No active workspace is open.", "OK");
            return;
        }

        var result = await _workspaceLifecycle.SaveAsync(_workspaceLifecycle.Current, cancellationToken);
        var name = result.Session?.Manifest.Name ?? _workspaceLifecycle.Current.Manifest.Name;
        await ApplyResultAsync("Save Workspace", result, $"Saved workspace '{name}'.", cancellationToken);
    }

    async Task ApplyResultAsync(string title, WorkspaceLifecycleResult result, string successMessage, CancellationToken cancellationToken)
    {
        var page = GetPage();
        if (!result.Succeeded)
        {
            if (page is not null)
                await page.DisplayAlert(title, FormatError(result), "OK");
            await RefreshAsync(cancellationToken);
            return;
        }

        await RefreshAsync(cancellationToken);
        if (page is not null)
            await page.DisplayAlert(title, successMessage, "OK");
    }

    void SetProjection(WorkspaceUiProjection projection)
    {
        Projection = projection;
        ProjectionChanged?.Invoke(this, EventArgs.Empty);
    }

    static async Task<string?> PickWorkspaceDirectoryAsync(Page page, CancellationToken cancellationToken)
    {
#if MACCATALYST
        var macFolder = await MainThread.InvokeOnMainThreadAsync(TryPickWorkspaceDirectoryWithMacOpenPanel);
        if (!string.IsNullOrWhiteSpace(macFolder))
            return macFolder;

        var picker = await MainThread.InvokeOnMainThreadAsync(() =>
        {
            var presenter = Platform.GetCurrentUIViewController();
            if (presenter is null)
                return null;

            var picker = new UIDocumentPickerViewController([UTTypes.Folder], false)
            {
                AllowsMultipleSelection = false
            };
            presenter.PresentViewController(picker, true, null);
            return picker;
        });

        if (picker is null)
            return null;

        return await AwaitPickedFolderAsync(picker, cancellationToken);
#else
        return await PickWorkspaceDirectoryFallbackAsync(page, cancellationToken);
#endif
    }

    static async Task<string?> PickWorkspaceDirectoryFallbackAsync(Page page, CancellationToken cancellationToken)
    {
        var defaultDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            "Documents",
            "Sailor",
            "SailorWorkspace");

        var directory = await page.DisplayPromptAsync(
            "New Workspace",
            "Workspace directory",
            "Create",
            "Cancel",
            "Path",
            -1,
            Keyboard.Text,
            defaultDirectory);

        return cancellationToken.IsCancellationRequested ? null : directory?.Trim();
    }

#if MACCATALYST
    static string? TryPickWorkspaceDirectoryWithMacOpenPanel()
    {
        var openPanelClass = ObjC.objc_getClass("NSOpenPanel");
        if (openPanelClass == IntPtr.Zero)
            return null;

        var panel = ObjC.SendIntPtr(openPanelClass, "openPanel");
        if (panel == IntPtr.Zero)
            return null;

        using var title = new NSString("New Workspace");
        using var message = new NSString("Choose an empty folder for the new Sailor workspace.");
        using var prompt = new NSString("Create");

        ObjC.SendBool(panel, "setAllowsMultipleSelection:", false);
        ObjC.SendBool(panel, "setCanChooseDirectories:", true);
        ObjC.SendBool(panel, "setCanChooseFiles:", false);
        ObjC.SendBool(panel, "setCanCreateDirectories:", true);
        ObjC.SendIntPtr(panel, "setTitle:", title.Handle);
        ObjC.SendIntPtr(panel, "setMessage:", message.Handle);
        ObjC.SendIntPtr(panel, "setPrompt:", prompt.Handle);

        const nint NSModalResponseOK = 1;
        if (ObjC.SendNInt(panel, "runModal") != NSModalResponseOK)
            return null;

        var urlHandle = ObjC.SendIntPtr(panel, "URL");
        if (urlHandle == IntPtr.Zero)
            return null;

        return ObjCRuntime.Runtime.GetNSObject<NSUrl>(urlHandle)?.Path;
    }

    static async Task<string?> AwaitPickedFolderAsync(UIDocumentPickerViewController picker, CancellationToken cancellationToken)
    {
        var completion = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
        var pickerDelegate = new WorkspaceFolderPickerDelegate(completion);
        picker.Delegate = pickerDelegate;

        using var registration = cancellationToken.Register(() =>
        {
            completion.TrySetResult(null);
            MainThread.BeginInvokeOnMainThread(() => picker.DismissViewController(true, null));
        });

        var selectedPath = await completion.Task;
        picker.Delegate = null;
        pickerDelegate.Dispose();
        picker.Dispose();
        return selectedPath;
    }

    sealed class WorkspaceFolderPickerDelegate : UIDocumentPickerDelegate
    {
        readonly TaskCompletionSource<string?> completion;

        public WorkspaceFolderPickerDelegate(TaskCompletionSource<string?> completion)
        {
            this.completion = completion;
        }

        public override void DidPickDocument(UIDocumentPickerViewController controller, NSUrl[] urls)
        {
            completion.TrySetResult(urls.FirstOrDefault()?.Path);
            controller.DismissViewController(true, null);
        }

        public override void WasCancelled(UIDocumentPickerViewController controller)
        {
            completion.TrySetResult(null);
            controller.DismissViewController(true, null);
        }
    }

    static class ObjC
    {
        const string ObjCLibrary = "/usr/lib/libobjc.A.dylib";

        [DllImport(ObjCLibrary)]
        public static extern IntPtr objc_getClass(string name);

        [DllImport(ObjCLibrary)]
        static extern IntPtr sel_registerName(string name);

        [DllImport(ObjCLibrary, EntryPoint = "objc_msgSend")]
        static extern IntPtr objc_msgSend_IntPtr(IntPtr receiver, IntPtr selector);

        [DllImport(ObjCLibrary, EntryPoint = "objc_msgSend")]
        static extern nint objc_msgSend_nint(IntPtr receiver, IntPtr selector);

        [DllImport(ObjCLibrary, EntryPoint = "objc_msgSend")]
        static extern void objc_msgSend_bool(IntPtr receiver, IntPtr selector, bool value);

        [DllImport(ObjCLibrary, EntryPoint = "objc_msgSend")]
        static extern void objc_msgSend_IntPtrArg(IntPtr receiver, IntPtr selector, IntPtr value);

        public static IntPtr SendIntPtr(IntPtr receiver, string selector)
            => objc_msgSend_IntPtr(receiver, sel_registerName(selector));

        public static nint SendNInt(IntPtr receiver, string selector)
            => objc_msgSend_nint(receiver, sel_registerName(selector));

        public static void SendBool(IntPtr receiver, string selector, bool value)
            => objc_msgSend_bool(receiver, sel_registerName(selector), value);

        public static void SendIntPtr(IntPtr receiver, string selector, IntPtr value)
            => objc_msgSend_IntPtrArg(receiver, sel_registerName(selector), value);
    }
#endif

    static Page? GetPage()
        => Microsoft.Maui.Controls.Shell.Current?.CurrentPage
            ?? Application.Current?.Windows.FirstOrDefault()?.Page
            ?? Application.Current?.MainPage;

    static string FormatError(WorkspaceLifecycleResult result)
    {
        if (!string.IsNullOrWhiteSpace(result.Error))
            return result.Error;

        if (result.Validation.Issues.Count > 0)
            return string.Join(Environment.NewLine, result.Validation.Issues.Select(x => $"{x.Field}: {x.Message}"));

        return "Workspace operation failed.";
    }

}
