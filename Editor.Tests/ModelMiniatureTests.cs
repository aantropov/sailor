using System.Buffers.Binary;
using SailorEditor.Services;
using SailorEditor.Workspace;

namespace Editor.Tests;

public sealed class ModelMiniatureTests
{
    const string ModelFileId =
        "{12191642-EE18-4046-BD44-5A4E6004B846}";
    const string UnbracedModelFileId =
        "12191642-EE18-4046-BD44-5A4E6004B846";

    [Fact]
    public void Resolve_UsesActiveWorkspaceCacheDirectory()
    {
        using var directory = new TempDirectory();
        var workspaceRoot = directory.CreateDirectory("Workspace");
        var contentDirectory =
            directory.CreateDirectory("Workspace", "Content");
        var cacheDirectory =
            directory.CreateDirectory("Workspace", "Generated Cache");
        var context = EngineLaunchContract.Resolve(
            workspaceRoot,
            null,
            contentDirectory,
            cacheDirectory,
            directory.Root,
            "workspace-model-miniatures");

        var resolved = ModelMiniaturePath.TryResolve(
            context.CacheDirectory,
            ModelFileId,
            out var miniaturePath);

        Assert.True(resolved);
        Assert.Equal(
            WorkspacePathPolicy.NormalizePhysicalPath(
                Path.Combine(
                    cacheDirectory,
                    ModelMiniaturePath.DirectoryName,
                    ModelFileId +
                    ModelMiniaturePath.FileExtension)),
            miniaturePath);
    }

    [Fact]
    public void Resolve_UsesEngineModeCacheDirectory()
    {
        using var directory = new TempDirectory();
        directory.CreateDirectory("Cache");
        var context = EngineLaunchContract.Resolve(
            null,
            null,
            null,
            null,
            directory.Root);

        var resolved = ModelMiniaturePath.TryResolve(
            context.CacheDirectory,
            ModelFileId,
            out var miniaturePath);

        Assert.True(resolved);
        Assert.Equal(
            WorkspacePathPolicy.NormalizePhysicalPath(
                Path.Combine(
                    directory.Root,
                    "Cache",
                    ModelMiniaturePath.DirectoryName,
                    ModelFileId +
                    ModelMiniaturePath.FileExtension)),
            miniaturePath);
    }

    [Fact]
    public void Resolve_PreservesUnbracedUnixFileId()
    {
        using var directory = new TempDirectory();

        var resolved = ModelMiniaturePath.TryResolve(
            directory.Root,
            UnbracedModelFileId,
            out var miniaturePath);

        Assert.True(resolved);
        Assert.Equal(
            WorkspacePathPolicy.NormalizePhysicalPath(
                Path.Combine(
                    directory.Root,
                    ModelMiniaturePath.DirectoryName,
                    UnbracedModelFileId +
                    ModelMiniaturePath.FileExtension)),
            miniaturePath);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("NullFileId")]
    [InlineData("../{12191642-EE18-4046-BD44-5A4E6004B846}")]
    [InlineData("{12191642-EE18-4046-BD44-5A4E6004B846}/preview")]
    public void Resolve_RejectsInvalidFileIds(string? fileId)
    {
        using var directory = new TempDirectory();

        Assert.False(
            ModelMiniaturePath.TryResolve(
                directory.Root,
                fileId,
                out var miniaturePath));
        Assert.Empty(miniaturePath);
    }

    [Fact]
    public void Loader_ReturnsDetachedBytesForExpectedPngEnvelope()
    {
        using var directory = new TempDirectory();
        Assert.True(
            ModelMiniaturePath.TryResolve(
                directory.Root,
                ModelFileId,
                out var miniaturePath));
        Directory.CreateDirectory(
            Path.GetDirectoryName(miniaturePath)!);
        var expected = CreatePngEnvelope(
            ModelMiniaturePath.Resolution,
            ModelMiniaturePath.Resolution);
        File.WriteAllBytes(miniaturePath, expected);

        var loaded = ModelMiniatureLoader.TryLoad(
            directory.Root,
            ModelFileId,
            out var actual);

        Assert.True(loaded);
        Assert.Equal(expected, actual);
        using var exclusive = new FileStream(
            miniaturePath,
            FileMode.Open,
            FileAccess.ReadWrite,
            FileShare.None);
        Assert.True(exclusive.CanRead);
    }

    [Fact]
    public void Loader_UsesFallbackForMissingOrInvalidPng()
    {
        using var directory = new TempDirectory();

        Assert.False(
            ModelMiniatureLoader.TryLoad(
                directory.Root,
                ModelFileId,
                out var missing));
        Assert.Empty(missing);

        Assert.True(
            ModelMiniaturePath.TryResolve(
                directory.Root,
                ModelFileId,
                out var miniaturePath));
        Directory.CreateDirectory(
            Path.GetDirectoryName(miniaturePath)!);
        File.WriteAllBytes(
            miniaturePath,
            CreatePngEnvelope(128, 256));

        Assert.False(
            ModelMiniatureLoader.TryLoad(
                directory.Root,
                ModelFileId,
                out var invalid));
        Assert.Empty(invalid);
    }

    [Fact]
    public void EditorModelInspector_LoadsRuntimeOnlyMiniatureAndKeepsFallback()
    {
        var viewModel = ReadRepositoryFile(
            "Editor",
            "ViewModels",
            "ModelFile.cs");
        var template = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "ModelFileTemplate.xaml");

        Assert.Contains(
            "public override Task<bool> LoadDependentResources()",
            viewModel,
            StringComparison.Ordinal);
        Assert.Contains(
            "GetLaunchContext()",
            viewModel,
            StringComparison.Ordinal);
        Assert.Contains(
            ".CacheDirectory",
            viewModel,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "EngineCacheDirectory",
            viewModel,
            StringComparison.Ordinal);
        Assert.Contains(
            "ModelMiniatureLoader.TryLoad(",
            viewModel,
            StringComparison.Ordinal);
        Assert.Contains(
            "[property: YamlIgnore]",
            viewModel,
            StringComparison.Ordinal);
        Assert.Contains(
            "LoadRuntimeDataWithoutDirtyTracking(() =>",
            viewModel,
            StringComparison.Ordinal);
        Assert.Contains(
            "IsLoaded = HasMiniature;",
            viewModel,
            StringComparison.Ordinal);

        Assert.Contains(
            "Source=\"box_24.png\"",
            template,
            StringComparison.Ordinal);
        Assert.Contains(
            "DataTrigger TargetType=\"Image\"",
            template,
            StringComparison.Ordinal);
        Assert.Contains(
            "Source=\"{Binding Miniature}\"",
            template,
            StringComparison.Ordinal);
        Assert.Contains(
            "WidthRequest=\"256\"",
            template,
            StringComparison.Ordinal);
        Assert.Contains(
            "HeightRequest=\"256\"",
            template,
            StringComparison.Ordinal);
        Assert.Contains(
            "Aspect=\"AspectFit\"",
            template,
            StringComparison.Ordinal);
    }

    static byte[] CreatePngEnvelope(uint width, uint height)
    {
        const int headerLength = 24;
        const int endChunkLength = 12;
        var bytes = new byte[headerLength + endChunkLength];
        new byte[]
        {
            0x89, 0x50, 0x4e, 0x47,
            0x0d, 0x0a, 0x1a, 0x0a
        }.CopyTo(bytes, 0);
        BinaryPrimitives.WriteUInt32BigEndian(
            bytes.AsSpan(8, 4),
            13);
        "IHDR"u8.CopyTo(bytes.AsSpan(12, 4));
        BinaryPrimitives.WriteUInt32BigEndian(
            bytes.AsSpan(16, 4),
            width);
        BinaryPrimitives.WriteUInt32BigEndian(
            bytes.AsSpan(20, 4),
            height);
        "IEND"u8.CopyTo(
            bytes.AsSpan(headerLength + 4, 4));
        return bytes;
    }

    static string ReadRepositoryFile(
        params string[] relativePath)
    {
        var current =
            new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var candidate = Path.Combine(
                relativePath
                    .Prepend(current.FullName)
                    .ToArray());
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }

            current = current.Parent;
        }

        throw new FileNotFoundException(
            $"Could not find repository file: " +
            $"{Path.Combine(relativePath)}");
    }

    sealed class TempDirectory : IDisposable
    {
        public TempDirectory()
        {
            Root = Path.Combine(
                Path.GetTempPath(),
                $"sailor-model-miniature-" +
                $"{Guid.NewGuid():N}");
            Directory.CreateDirectory(Root);
        }

        public string Root { get; }

        public string CreateDirectory(
            params string[] relativePath)
        {
            var path = Path.Combine(
                relativePath.Prepend(Root).ToArray());
            Directory.CreateDirectory(path);
            return path;
        }

        public void Dispose()
        {
            if (Directory.Exists(Root))
            {
                Directory.Delete(Root, recursive: true);
            }
        }
    }
}
