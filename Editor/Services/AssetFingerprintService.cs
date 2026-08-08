using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using SkiaSharp;

namespace SailorEditor.Services;

sealed class AssetFingerprintService(EngineService engineService)
{
    const int FingerprintSize = 256;
    readonly SemaphoreSlim fingerprintWriteLock = new(1, 1);

    public string? GetFingerprintPath(FileId? fileId)
    {
        var value = fileId?.Value;
        if (string.IsNullOrWhiteSpace(value))
            return null;

        var filename = value + ".png";
        if (!string.Equals(
                Path.GetFileName(filename),
                filename,
                StringComparison.Ordinal))
        {
            return null;
        }

        return Path.Combine(
            engineService.GetLaunchContext().CacheDirectory,
            "Fingerprints",
            filename);
    }

    public ImageSource? TryGetCachedPreview(AssetFile asset)
    {
        if (asset is ModelFile { Fingerprint: not null } model)
            return model.Fingerprint;
        if (asset is TextureFile { Texture: not null } texture &&
            !texture.Texture.IsEmpty)
        {
            return texture.Texture;
        }

        var fingerprintPath = GetFingerprintPath(asset.FileId);
        return fingerprintPath is not null && File.Exists(fingerprintPath)
            ? ImageSource.FromFile(fingerprintPath)
            : null;
    }

    public async Task<ImageSource?> LoadPreviewAsync(
        AssetFile asset,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(asset);
        await asset.EnsureMetadataLoadedAsync(cancellationToken);

        if (asset is TextureFile texture)
        {
            await texture.LoadDependentResources(cancellationToken);
            return texture.Texture;
        }

        if (asset is ModelFile model)
        {
            await model.LoadDependentResources();
            return model.Fingerprint;
        }

        return TryGetCachedPreview(asset);
    }

    public async Task<ImageSource?> LoadTexturePreviewAsync(
        TextureFile texture,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(texture);
        var fingerprintPath = GetFingerprintPath(texture.FileId);

        if (texture.GlbTextureIndex >= 0)
        {
            var generatedPath = await EnsureGlbFingerprintAsync(
                texture,
                fingerprintPath,
                cancellationToken);
            if (generatedPath is not null)
                return ImageSource.FromFile(generatedPath);
        }
        else if (texture.Asset?.Exists == true &&
                 await CanDecodeImageAsync(
                     texture.Asset.FullName,
                     cancellationToken))
        {
            return ImageSource.FromFile(texture.Asset.FullName);
        }

        return fingerprintPath is not null && File.Exists(fingerprintPath)
            ? ImageSource.FromFile(fingerprintPath)
            : null;
    }

    async Task<string?> EnsureGlbFingerprintAsync(
        TextureFile texture,
        string? fingerprintPath,
        CancellationToken cancellationToken)
    {
        if (fingerprintPath is null ||
            texture.Asset?.Exists != true ||
            texture.GlbTextureIndex < 0)
        {
            return null;
        }

        if (IsFingerprintCurrent(fingerprintPath, texture.Asset.FullName))
            return fingerprintPath;

        await fingerprintWriteLock.WaitAsync(cancellationToken);
        try
        {
            if (IsFingerprintCurrent(fingerprintPath, texture.Asset.FullName))
                return fingerprintPath;

            var generated = false;
            try
            {
                generated = await Task.Run(
                    () => GenerateGlbFingerprint(
                        texture.Asset.FullName,
                        texture.GlbTextureIndex,
                        fingerprintPath,
                        cancellationToken),
                    cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(
                    $"Could not generate GLB texture fingerprint for " +
                    $"{texture.FileId}: {exception.Message}");
            }

            return generated || File.Exists(fingerprintPath)
                ? fingerprintPath
                : null;
        }
        finally
        {
            fingerprintWriteLock.Release();
        }
    }

    static bool IsFingerprintCurrent(
        string fingerprintPath,
        string sourcePath)
    {
        try
        {
            return File.Exists(fingerprintPath) &&
                File.GetLastWriteTimeUtc(fingerprintPath) >=
                File.GetLastWriteTimeUtc(sourcePath);
        }
        catch
        {
            return false;
        }
    }

    static Task<bool> CanDecodeImageAsync(
        string imagePath,
        CancellationToken cancellationToken)
        => Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            using var codec = SKCodec.Create(imagePath);
            return codec is not null;
        }, cancellationToken);

    static bool GenerateGlbFingerprint(
        string glbPath,
        int textureIndex,
        string fingerprintPath,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!GlbExtractor.ExtractTextureFromGLB(
                glbPath,
                textureIndex,
                out var textureStream) ||
            textureStream is null)
        {
            return false;
        }

        using (textureStream)
        using (var original = SKBitmap.Decode(textureStream))
        {
            if (original is null || original.Width <= 0 || original.Height <= 0)
                return false;

            var scale = Math.Min(
                1f,
                FingerprintSize / (float)Math.Max(original.Width, original.Height));
            var width = Math.Max(1, (int)Math.Round(original.Width * scale));
            var height = Math.Max(1, (int)Math.Round(original.Height * scale));
            using var resized = original.Resize(
                new SKImageInfo(width, height),
                SKFilterQuality.High);
            if (resized is null)
                return false;

            using var image = SKImage.FromBitmap(resized);
            using var encoded = image.Encode(SKEncodedImageFormat.Png, 90);
            if (encoded is null || encoded.Size == 0)
                return false;

            Directory.CreateDirectory(Path.GetDirectoryName(fingerprintPath)!);
            var temporaryPath = fingerprintPath + "." +
                Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var stream = new FileStream(
                    temporaryPath,
                    FileMode.CreateNew,
                    FileAccess.Write,
                    FileShare.None))
                {
                    encoded.SaveTo(stream);
                    stream.Flush(true);
                }

                File.Move(temporaryPath, fingerprintPath, true);
                return true;
            }
            finally
            {
                if (File.Exists(temporaryPath))
                    File.Delete(temporaryPath);
            }
        }
    }
}
