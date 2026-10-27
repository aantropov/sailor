using System.ComponentModel;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Controls;

public sealed class AssetPreviewImage : Image
{
    readonly object previewGate = new();
    CancellationTokenSource? previewCancellation;
    AssetFile? observedAsset;
    AssetsService? assetsService;
    EngineService? engineService;
    bool loaded;
    long revision;

    public static readonly BindableProperty FileIdProperty =
        BindableProperty.Create(
            nameof(FileId), typeof(FileId), typeof(AssetPreviewImage), new FileId(),
            propertyChanged: OnFileIdChanged);

    public FileId FileId
    {
        get => (FileId)GetValue(FileIdProperty);
        set => SetValue(FileIdProperty, value);
    }

    public AssetPreviewImage()
    {
        Aspect = Aspect.AspectFit;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    void OnLoaded(object? sender, EventArgs args)
    {
        if (loaded)
        {
            return;
        }
        loaded = true;
        assetsService = MauiProgram.GetService<AssetsService>();
        engineService = MauiProgram.GetService<EngineService>();
        assetsService.Changed += OnAssetsChanged;
        engineService.OnLifecycleStateChanged += OnEngineStateChanged;
        QueuePreviewRefresh();
    }

    void OnUnloaded(object? sender, EventArgs args)
    {
        loaded = false;
        CancelPendingPreview();
        ObserveAsset(null);
        if (assetsService is not null)
        {
            assetsService.Changed -= OnAssetsChanged;
        }
        if (engineService is not null)
        {
            engineService.OnLifecycleStateChanged -= OnEngineStateChanged;
        }
        Source = null;
    }

    static void OnFileIdChanged(BindableObject bindable, object oldValue, object newValue)
    {
        var image = (AssetPreviewImage)bindable;
        image.Source = null;
        image.ObserveAsset(null);
        image.IsVisible = newValue is FileId fileId && !fileId.IsEmpty();
        image.QueuePreviewRefresh();
    }

    void OnAssetsChanged() => Dispatcher.Dispatch(() =>
    {
        Source = null;
        QueuePreviewRefresh();
    });

    void OnEngineStateChanged(EngineLifecycleState state) => Dispatcher.Dispatch(() =>
    {
        Source = null;
        QueuePreviewRefresh();
    });

    void ObserveAsset(AssetFile? asset)
    {
        if (ReferenceEquals(observedAsset, asset))
        {
            return;
        }
        if (observedAsset is not null)
        {
            observedAsset.PropertyChanged -= OnAssetChanged;
        }
        observedAsset = asset;
        if (observedAsset is not null)
        {
            observedAsset.PropertyChanged += OnAssetChanged;
        }
    }

    void OnAssetChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (sender is MaterialFile && args.PropertyName is not
            (nameof(AssetFile.IsDirty) or nameof(AssetFile.CanOpenAssetFile) or
             nameof(AssetFile.IsLoaded) or nameof(AssetFile.LoadError) or nameof(AssetFile.LoadWarning)))
        {
            Dispatcher.Dispatch(QueuePreviewRefresh);
        }
    }

    void QueuePreviewRefresh()
    {
        CancelPendingPreview();
        if (!loaded || FileId is null || FileId.IsEmpty())
        {
            return;
        }
        IsVisible = true;
        var cancellation = new CancellationTokenSource();
        lock (previewGate)
        {
            previewCancellation = cancellation;
        }
        _ = RefreshPreviewAsync(FileId.Value, revision, assetsService!.WorkspaceEpoch,
            engineService!.Generation, cancellation);
    }

    async Task RefreshPreviewAsync(string requestedFileId, long requestedRevision,
        long workspaceEpoch, long engineGeneration, CancellationTokenSource cancellation)
    {
        try
        {
            var asset = await assetsService!.ResolveAssetAsync(new FileId(requestedFileId), cancellation.Token);
            if (asset is not null)
            {
                await asset.EnsureMetadataLoadedAsync(cancellation.Token);
                if (!IsCurrent())
                {
                    return;
                }
                ObserveAsset(asset);
            }
            var preview = asset is null ? null : await MauiProgram.GetService<AssetFingerprintService>()
                .LoadPreviewAsync(asset, cancellation.Token);
            if (!IsCurrent())
            {
                return;
            }
            Dispatcher.Dispatch(() =>
            {
                if (!IsCurrent())
                {
                    return;
                }
                ObserveAsset(asset);
                Source = preview;
                IsVisible = preview is not null && !preview.IsEmpty;
            });
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"Asset preview failed for {requestedFileId}: {exception.Message}");
        }
        finally
        {
            lock (previewGate)
            {
                if (ReferenceEquals(previewCancellation, cancellation))
                {
                    previewCancellation = null;
                }
                cancellation.Dispose();
            }
        }

        bool IsCurrent() => loaded && !cancellation.IsCancellationRequested &&
            revision == requestedRevision && workspaceEpoch == assetsService!.WorkspaceEpoch &&
            engineGeneration == engineService!.Generation &&
            string.Equals(FileId?.Value, requestedFileId, StringComparison.Ordinal);
    }

    void CancelPendingPreview()
    {
        revision++;
        lock (previewGate)
        {
            previewCancellation?.Cancel();
            previewCancellation = null;
        }
    }
}
