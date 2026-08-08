using SailorEditor.Services;
using SailorEngine;

namespace SailorEditor.Controls;

public sealed class AssetPreviewImage : Image
{
    CancellationTokenSource? previewCancellation;

    public static readonly BindableProperty FileIdProperty =
        BindableProperty.Create(
            nameof(FileId),
            typeof(FileId),
            typeof(AssetPreviewImage),
            new FileId(),
            propertyChanged: OnFileIdChanged);

    public FileId FileId
    {
        get => (FileId)GetValue(FileIdProperty);
        set => SetValue(FileIdProperty, value);
    }

    public AssetPreviewImage()
    {
        Aspect = Aspect.AspectFit;
        IsVisible = false;
        Unloaded += (_, _) => CancelPendingPreview();
    }

    static void OnFileIdChanged(
        BindableObject bindable,
        object oldValue,
        object newValue)
        => ((AssetPreviewImage)bindable).QueuePreviewRefresh();

    void QueuePreviewRefresh()
    {
        CancelPendingPreview();
        Source = null;
        IsVisible = false;
        if (FileId is null || FileId.IsEmpty())
            return;

        var cancellation = new CancellationTokenSource();
        previewCancellation = cancellation;
        _ = RefreshPreviewAsync(FileId.Value, cancellation);
    }

    async Task RefreshPreviewAsync(
        string requestedFileId,
        CancellationTokenSource cancellation)
    {
        try
        {
            var asset = await MauiProgram.GetService<AssetsService>()
                .ResolveAssetAsync(
                    new FileId(requestedFileId),
                    cancellation.Token);
            var preview = asset is null
                ? null
                : await MauiProgram.GetService<AssetFingerprintService>()
                    .LoadPreviewAsync(asset, cancellation.Token);
            if (cancellation.IsCancellationRequested ||
                !string.Equals(FileId?.Value, requestedFileId, StringComparison.Ordinal))
            {
                return;
            }

            Dispatcher.Dispatch(() =>
            {
                if (cancellation.IsCancellationRequested ||
                    !string.Equals(FileId?.Value, requestedFileId, StringComparison.Ordinal))
                {
                    return;
                }

                Source = preview;
                IsVisible = preview is not null && !preview.IsEmpty;
            });
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Asset preview failed for {requestedFileId}: {exception.Message}");
        }
        finally
        {
            if (ReferenceEquals(previewCancellation, cancellation))
                previewCancellation = null;
            cancellation.Dispose();
        }
    }

    void CancelPendingPreview()
        => Interlocked.Exchange(ref previewCancellation, null)?.Cancel();
}
