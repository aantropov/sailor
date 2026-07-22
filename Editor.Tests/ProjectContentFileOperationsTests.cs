using System.Diagnostics;
using SailorEditor.Content;
using YamlDotNet.RepresentationModel;

namespace Editor.Tests;

public sealed class ProjectContentFileOperationsTests : IDisposable
{
    readonly string testRoot = Path.Combine(
        Path.GetTempPath(),
        "sailor-content-file-operations",
        Guid.NewGuid().ToString("N"));
    readonly string contentRoot;
    readonly string engineContentRoot;

    public ProjectContentFileOperationsTests()
    {
        contentRoot = Path.Combine(testRoot, "Workspace", "Content");
        engineContentRoot = Path.Combine(testRoot, "Engine", "Content");
        Directory.CreateDirectory(contentRoot);
        Directory.CreateDirectory(engineContentRoot);
    }

    [Fact]
    public void MoveAssetGroup_MovesSourceAndEverySidecarThatResolvesToIt()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(contentRoot, "Archive")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var unrelatedSource = WriteSource(sourceDirectory, "Other.png", "other");
        var unrelatedMetadata = WriteMetadata(sourceDirectory, "Other.png.asset", "Other.png", "{OTHER}");

        var result = new ProjectContentFileOperations().MoveAssetGroup(
            contentRoot,
            group.SecondaryAssetInfoPaths[0],
            destination);

        Assert.True(result.Succeeded, result.Error);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        Assert.False(File.Exists(group.SourcePath));
        Assert.All(group.AssetInfoPaths, path => Assert.False(File.Exists(path)));

        var movedSource = Path.Combine(destination, "Duck.glb");
        Assert.True(File.Exists(movedSource));
        foreach (var originalMetadata in group.AssetInfoPaths)
        {
            var movedMetadata = Path.Combine(destination, Path.GetFileName(originalMetadata));
            Assert.True(File.Exists(movedMetadata));
            Assert.Equal("Duck.glb", ReadDeclaredFilename(movedMetadata));
            Assert.True(AssetSourcePathContract.TryResolve(
                movedMetadata,
                "Duck.glb",
                out var resolution,
                out var error), error);
            Assert.True(ProjectContentPathPolicy.IsSamePath(movedSource, resolution.SourcePath));
        }

        Assert.True(File.Exists(unrelatedSource));
        Assert.True(File.Exists(unrelatedMetadata));
    }

    [Fact]
    public void MoveAssetGroup_RejectsDestinationCollisionBeforeChangingDisk()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(contentRoot, "Archive")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var collisionPath = Path.Combine(destination, Path.GetFileName(group.SecondaryAssetInfoPaths[0]));
        File.WriteAllText(collisionPath, "collision");
        var originalContents = Snapshot(group.AllPaths);

        var result = new ProjectContentFileOperations().MoveAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            destination);

        Assert.False(result.Succeeded);
        Assert.Contains("already exists", result.Error, StringComparison.OrdinalIgnoreCase);
        AssertFilesEqual(originalContents);
        Assert.Equal("collision", File.ReadAllText(collisionPath));
        Assert.False(File.Exists(Path.Combine(destination, Path.GetFileName(group.SourcePath))));
    }

    [Fact]
    public void MoveAssetGroup_RollsBackCompletedMovesWhenALaterMoveFails()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(contentRoot, "Archive")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var originalContents = Snapshot(group.AllPaths);
        var fileSystem = new FaultInjectingFileSystem
        {
            ThrowOnMoveNumber = 3
        };

        var result = new ProjectContentFileOperations(fileSystem).MoveAssetGroup(
            contentRoot,
            group.SecondaryAssetInfoPaths[1],
            destination);

        Assert.False(result.Succeeded);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        AssertFilesEqual(originalContents);
        Assert.Empty(Directory.EnumerateFileSystemEntries(destination));
    }

    [Fact]
    public void MoveAssetGroup_ReportsAmbiguousSourceAndDestinationAfterPartialMoveFailure()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(contentRoot, "Archive")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var fileSystem = new FaultInjectingFileSystem
        {
            CopyThenThrowOnMoveNumber = 1
        };

        var result = new ProjectContentFileOperations(fileSystem).MoveAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            destination);

        Assert.False(result.Succeeded);
        Assert.False(result.RollbackSucceeded);
        Assert.Contains(result.UnrestoredPaths, path => ProjectContentPathPolicy.IsSamePath(path, group.SourcePath));
        Assert.True(File.Exists(group.SourcePath));
        Assert.True(File.Exists(Path.Combine(destination, Path.GetFileName(group.SourcePath))));
    }

    [Fact]
    public void DuplicateAssetGroup_CopiesEverySidecarWithFreshFileIdsAndReboundReferences()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var originalFileIds = group.AssetInfoPaths.ToDictionary(
            path => path,
            ReadFileId,
            ProjectContentPathPolicy.PathComparer);
        const string externalFileId = "{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";
        File.AppendAllText(
            group.PrimaryAssetInfoPath,
            $"references:{Environment.NewLine}" +
            $"  - \"{originalFileIds[group.SecondaryAssetInfoPaths[0]]}\"{Environment.NewLine}" +
            $"nested:{Environment.NewLine}" +
            $"  texture: \"{originalFileIds[group.SecondaryAssetInfoPaths[1]]}\"{Environment.NewLine}" +
            $"external: \"{externalFileId}\"{Environment.NewLine}");
        var originalContents = Snapshot(group.AllPaths);
        var unrelatedSource = WriteSource(sourceDirectory, "Other.png", "other");
        var unrelatedMetadata = WriteMetadata(sourceDirectory, "Other.png.asset", "Other.png", "{OTHER}");

        var result = new ProjectContentFileOperations().DuplicateAssetGroup(
            contentRoot,
            group.SecondaryAssetInfoPaths[0]);

        Assert.True(result.Succeeded, result.Error);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        var copiedSource = Path.Combine(sourceDirectory, "Duck Copy.glb");
        var copiedMetadataPaths = group.AssetInfoPaths
            .Select(path => Path.Combine(
                sourceDirectory,
                Path.GetFileName(path).Replace("Duck.glb", "Duck Copy.glb", StringComparison.Ordinal)))
            .ToArray();
        var copiedPrimary = copiedSource + ".asset";
        Assert.NotNull(result.CreatedAssetInfoPath);
        Assert.True(ProjectContentPathPolicy.IsSamePath(copiedPrimary, result.CreatedAssetInfoPath));
        Assert.Equal(ReadFileId(copiedPrimary), result.CreatedFileId);
        Assert.Equal(File.ReadAllText(group.SourcePath), File.ReadAllText(copiedSource));
        Assert.All(copiedMetadataPaths, path => Assert.True(File.Exists(path), $"Expected copied sidecar: {path}"));
        AssertFilesEqual(originalContents);
        Assert.True(File.Exists(unrelatedSource));
        Assert.True(File.Exists(unrelatedMetadata));

        var copiedFileIds = copiedMetadataPaths.ToDictionary(
            path => path,
            ReadFileId,
            ProjectContentPathPolicy.PathComparer);
        Assert.Equal(copiedFileIds.Count, copiedFileIds.Values.Distinct(StringComparer.OrdinalIgnoreCase).Count());
        Assert.All(copiedFileIds.Values, fileId =>
        {
            Assert.True(Guid.TryParse(fileId, out _), $"Expected a GUID FileId, got: {fileId}");
            Assert.DoesNotContain(fileId, originalFileIds.Values, StringComparer.OrdinalIgnoreCase);
        });

        foreach (var copiedMetadataPath in copiedMetadataPaths)
        {
            Assert.Equal("Duck Copy.glb", ReadDeclaredFilename(copiedMetadataPath));
            Assert.Equal("Sailor::TextureAssetInfo", ReadScalar(copiedMetadataPath, "assetInfoType"));
        }

        var copiedPrimaryRoot = ReadMetadataRoot(copiedPrimary);
        var references = Assert.IsType<YamlSequenceNode>(copiedPrimaryRoot.Children[new YamlScalarNode("references")]);
        Assert.Equal(
            copiedFileIds[Path.Combine(sourceDirectory, "Duck Copy.glb_BaseColor.asset")],
            Assert.IsType<YamlScalarNode>(Assert.Single(references.Children)).Value);
        var nested = Assert.IsType<YamlMappingNode>(copiedPrimaryRoot.Children[new YamlScalarNode("nested")]);
        Assert.Equal(
            copiedFileIds[Path.Combine(sourceDirectory, "Duck Copy.glb_BaseColor.png.asset")],
            Assert.IsType<YamlScalarNode>(nested.Children[new YamlScalarNode("texture")]).Value);
        Assert.Equal(externalFileId, ReadScalar(copiedPrimary, "external"));
    }

    [Fact]
    public void DuplicateAssetGroup_AdvancesCopyNumberWhenAnyGroupDestinationCollides()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var collision = Path.Combine(sourceDirectory, "Duck Copy.glb_BaseColor.asset");
        File.WriteAllText(collision, "collision");

        var result = new ProjectContentFileOperations().DuplicateAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath);

        Assert.True(result.Succeeded, result.Error);
        Assert.NotNull(result.CreatedAssetInfoPath);
        Assert.True(ProjectContentPathPolicy.IsSamePath(
            Path.Combine(sourceDirectory, "Duck Copy 2.glb.asset"),
            result.CreatedAssetInfoPath));
        Assert.True(File.Exists(Path.Combine(sourceDirectory, "Duck Copy 2.glb")));
        Assert.True(File.Exists(Path.Combine(sourceDirectory, "Duck Copy 2.glb_BaseColor.asset")));
        Assert.False(File.Exists(Path.Combine(sourceDirectory, "Duck Copy.glb")));
        Assert.Equal("collision", File.ReadAllText(collision));
    }

    [Fact]
    public void DuplicateAssetGroup_RollsBackEveryCreatedFileAfterMetadataWriteFailure()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var originalContents = Snapshot(group.AllPaths);
        var fileSystem = new FaultInjectingFileSystem
        {
            ThrowOnWriteNumber = 2
        };

        var result = new ProjectContentFileOperations(fileSystem).DuplicateAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath);

        Assert.False(result.Succeeded);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        AssertFilesEqual(originalContents);
        Assert.DoesNotContain(
            Directory.EnumerateFiles(sourceDirectory),
            path => Path.GetFileName(path).StartsWith("Duck Copy", StringComparison.Ordinal));
    }

    [Fact]
    public void DuplicateAssetGroup_ReportsDestinationThatCouldNotBeRemovedDuringRollback()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var originalContents = Snapshot(group.AllPaths);
        var fileSystem = new FaultInjectingFileSystem
        {
            WriteThenThrowOnWriteNumber = 1
        };

        var result = new ProjectContentFileOperations(fileSystem).DuplicateAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath);

        Assert.False(result.Succeeded);
        Assert.False(result.RollbackSucceeded);
        var unrestored = Assert.Single(result.UnrestoredPaths);
        Assert.True(ProjectContentPathPolicy.IsSamePath(
            Path.Combine(sourceDirectory, "Duck Copy.glb.asset"),
            unrestored));
        Assert.True(File.Exists(unrestored));
        Assert.False(File.Exists(Path.Combine(sourceDirectory, "Duck Copy.glb")));
        AssertFilesEqual(originalContents);
    }

    [Fact]
    public void RenameAssetGroup_RenamesSourceAndPrimarySidecarAndUpdatesEveryFilename()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var fileIds = group.AssetInfoPaths.ToDictionary(
            path => Path.GetFileName(path)!,
            ReadFileId,
            StringComparer.Ordinal);

        var result = new ProjectContentFileOperations().RenameAssetGroup(
            contentRoot,
            group.SecondaryAssetInfoPaths[0],
            "Bird");

        Assert.True(result.Succeeded, result.Error);
        var renamedSource = Path.Combine(sourceDirectory, "Bird.glb");
        var renamedPrimary = renamedSource + ".asset";
        Assert.True(File.Exists(renamedSource));
        Assert.True(File.Exists(renamedPrimary));
        Assert.False(File.Exists(group.SourcePath));
        Assert.False(File.Exists(group.PrimaryAssetInfoPath));

        var finalMetadataPaths = group.SecondaryAssetInfoPaths.Append(renamedPrimary).ToArray();
        foreach (var metadataPath in finalMetadataPaths)
        {
            Assert.True(File.Exists(metadataPath));
            Assert.Equal("Bird.glb", ReadDeclaredFilename(metadataPath));
        }

        Assert.Equal(fileIds[Path.GetFileName(group.PrimaryAssetInfoPath)], ReadFileId(renamedPrimary));
        foreach (var secondaryPath in group.SecondaryAssetInfoPaths)
            Assert.Equal(fileIds[Path.GetFileName(secondaryPath)], ReadFileId(secondaryPath));
    }

    [Fact]
    public void RenameAssetGroup_RejectsPrimarySidecarCollisionBeforeChangingDisk()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var collision = Path.Combine(sourceDirectory, "Bird.glb.asset");
        File.WriteAllText(collision, "collision");
        var originalContents = Snapshot(group.AllPaths);

        var result = new ProjectContentFileOperations().RenameAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            "Bird");

        Assert.False(result.Succeeded);
        AssertFilesEqual(originalContents);
        Assert.Equal("collision", File.ReadAllText(collision));
        Assert.False(File.Exists(Path.Combine(sourceDirectory, "Bird.glb")));
    }

    [Fact]
    public void RenameAssetGroup_RollsBackMovesAndMetadataWhenAWriteFails()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var originalContents = Snapshot(group.AllPaths);
        var fileSystem = new FaultInjectingFileSystem
        {
            ThrowOnWriteNumber = 2
        };

        var result = new ProjectContentFileOperations(fileSystem).RenameAssetGroup(
            contentRoot,
            group.SecondaryAssetInfoPaths[0],
            "Bird");

        Assert.False(result.Succeeded);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        AssertFilesEqual(originalContents);
        Assert.False(File.Exists(Path.Combine(sourceDirectory, "Bird.glb")));
        Assert.False(File.Exists(Path.Combine(sourceDirectory, "Bird.glb.asset")));
    }

    [Fact]
    public void DeleteAssetGroup_DeletesOnlyTheResolvedSourceGroup()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var unrelatedSource = WriteSource(sourceDirectory, "Other.png", "other");
        var unrelatedMetadata = WriteMetadata(sourceDirectory, "Other.png.asset", "Other.png", "{OTHER}");

        var result = new ProjectContentFileOperations().DeleteAssetGroup(
            contentRoot,
            group.SecondaryAssetInfoPaths[1]);

        Assert.True(result.Succeeded, result.Error);
        Assert.All(group.AllPaths, path => Assert.False(File.Exists(path)));
        Assert.True(File.Exists(unrelatedSource));
        Assert.True(File.Exists(unrelatedMetadata));
        Assert.DoesNotContain(
            Directory.EnumerateDirectories(sourceDirectory),
            path => Path.GetFileName(path).StartsWith(".sailor-asset-", StringComparison.Ordinal));
    }

    [Fact]
    public void DeleteAssetGroup_RollsBackStagingMovesWhenAMoveFails()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var originalContents = Snapshot(group.AllPaths);
        var fileSystem = new FaultInjectingFileSystem
        {
            ThrowOnMoveNumber = 2
        };

        var result = new ProjectContentFileOperations(fileSystem).DeleteAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath);

        Assert.False(result.Succeeded);
        Assert.True(result.RollbackSucceeded);
        AssertFilesEqual(originalContents);
        Assert.DoesNotContain(
            Directory.EnumerateDirectories(sourceDirectory),
            path => Path.GetFileName(path).StartsWith(".sailor-asset-", StringComparison.Ordinal));
    }

    [Fact]
    public void AssetOperations_RejectEngineFilesWhenWorkspaceContentIsTheWritableRoot()
    {
        var engineDirectory = Directory.CreateDirectory(Path.Combine(engineContentRoot, "Models")).FullName;
        var workspaceDestination = Directory.CreateDirectory(Path.Combine(contentRoot, "Imported")).FullName;
        var group = WriteAssetGroup(engineDirectory, "EngineOnly.glb");

        var result = new ProjectContentFileOperations().MoveAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            workspaceDestination);

        Assert.False(result.Succeeded);
        Assert.Contains("outside", result.Error, StringComparison.OrdinalIgnoreCase);
        Assert.All(group.AllPaths, path => Assert.True(File.Exists(path)));
    }

    [Fact]
    public void DuplicateAssetGroup_RejectsReadOnlyMountAndPathThatEscapesThroughDirectoryLink()
    {
        var engineDirectory = Directory.CreateDirectory(Path.Combine(engineContentRoot, "Models")).FullName;
        var engineGroup = WriteAssetGroup(engineDirectory, "EngineOnly.glb");
        var externalDirectory = Directory.CreateDirectory(Path.Combine(testRoot, "ExternalDuplicate")).FullName;
        var externalGroup = WriteAssetGroup(externalDirectory, "Linked.glb");
        var sourceLink = Path.Combine(contentRoot, "ExternalDuplicateLink");
        CreateDirectoryLink(sourceLink, externalDirectory);
        var operations = new ProjectContentFileOperations();

        var readOnlyResult = operations.DuplicateAssetGroup(
            contentRoot,
            engineGroup.PrimaryAssetInfoPath);
        var linkedResult = operations.DuplicateAssetGroup(
            contentRoot,
            Path.Combine(sourceLink, Path.GetFileName(externalGroup.PrimaryAssetInfoPath)));

        Assert.False(readOnlyResult.Succeeded);
        Assert.Contains("outside", readOnlyResult.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(linkedResult.Succeeded);
        Assert.Contains("outside", linkedResult.Error, StringComparison.OrdinalIgnoreCase);
        Assert.All(engineGroup.AllPaths, path => Assert.True(File.Exists(path)));
        Assert.All(externalGroup.AllPaths, path => Assert.True(File.Exists(path)));
        Assert.DoesNotContain(
            Directory.EnumerateFiles(engineDirectory),
            path => Path.GetFileName(path).StartsWith("EngineOnly Copy", StringComparison.Ordinal));
        Assert.DoesNotContain(
            Directory.EnumerateFiles(externalDirectory),
            path => Path.GetFileName(path).StartsWith("Linked Copy", StringComparison.Ordinal));
    }

    [Fact]
    public void AssetOperations_AllowEngineFilesWhenEngineContentIsTheWritableRoot()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(engineContentRoot, "Models")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(engineContentRoot, "Archive")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "EngineOnly.glb");

        var result = new ProjectContentFileOperations().MoveAssetGroup(
            engineContentRoot,
            group.PrimaryAssetInfoPath,
            destination);

        Assert.True(result.Succeeded, result.Error);
        Assert.True(File.Exists(Path.Combine(destination, "EngineOnly.glb")));
    }

    [Fact]
    public void MoveAssetGroup_RejectsDestinationThatEscapesThroughDirectoryLink()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var externalDirectory = Directory.CreateDirectory(Path.Combine(testRoot, "External")).FullName;
        var destinationLink = Path.Combine(contentRoot, "ExternalLink");
        CreateDirectoryLink(destinationLink, externalDirectory);
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");

        var result = new ProjectContentFileOperations().MoveAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            destinationLink);

        Assert.False(result.Succeeded);
        Assert.Contains("outside", result.Error, StringComparison.OrdinalIgnoreCase);
        Assert.All(group.AllPaths, path => Assert.True(File.Exists(path)));
        Assert.Empty(Directory.EnumerateFileSystemEntries(externalDirectory));
    }

    [Fact]
    public void CreateFolder_CreatesPortableSubfolderInWritableContent()
    {
        var parent = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;

        var result = new ProjectContentFileOperations().CreateFolder(
            contentRoot,
            parent,
            "Bird Models");

        Assert.True(result.Succeeded, result.Error);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        Assert.True(Directory.Exists(Path.Combine(parent, "Bird Models")));
    }

    [Fact]
    public void CreateFolder_AllowsWritableContentRootAsParent()
    {
        var result = new ProjectContentFileOperations().CreateFolder(
            contentRoot,
            contentRoot,
            "Models");

        Assert.True(result.Succeeded, result.Error);
        Assert.True(Directory.Exists(Path.Combine(contentRoot, "Models")));
    }

    [Fact]
    public void CreateFolder_RejectsInvalidNamesAndExistingDestinations()
    {
        var parent = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        Directory.CreateDirectory(Path.Combine(parent, "ExistingFolder"));
        File.WriteAllText(Path.Combine(parent, "ExistingFile"), "collision");
        var operations = new ProjectContentFileOperations();

        foreach (var invalidName in new[] { "", "..", "../Outside", "Nested/Outside", "Nested\\Outside", "CON" })
        {
            var result = operations.CreateFolder(contentRoot, parent, invalidName);
            Assert.False(result.Succeeded);
        }

        var directoryCollision = operations.CreateFolder(contentRoot, parent, "ExistingFolder");
        var fileCollision = operations.CreateFolder(contentRoot, parent, "ExistingFile");

        Assert.False(directoryCollision.Succeeded);
        Assert.Contains("already exists", directoryCollision.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(fileCollision.Succeeded);
        Assert.Contains("already exists", fileCollision.Error, StringComparison.OrdinalIgnoreCase);
        Assert.Empty(Directory.EnumerateDirectories(parent, "Outside", SearchOption.TopDirectoryOnly));
    }

    [Fact]
    public void CreateFolder_RejectsParentOutsideWritableRootOrThroughDirectoryLink()
    {
        var outside = Directory.CreateDirectory(Path.Combine(testRoot, "OutsideCreate")).FullName;
        var parentLink = Path.Combine(contentRoot, "ExternalCreateLink");
        CreateDirectoryLink(parentLink, outside);
        var operations = new ProjectContentFileOperations();

        var outsideResult = operations.CreateFolder(contentRoot, outside, "OutsideChild");
        var linkedResult = operations.CreateFolder(contentRoot, parentLink, "LinkedChild");

        Assert.False(outsideResult.Succeeded);
        Assert.Contains("outside", outsideResult.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(linkedResult.Succeeded);
        Assert.Contains("outside", linkedResult.Error, StringComparison.OrdinalIgnoreCase);
        Assert.Empty(Directory.EnumerateFileSystemEntries(outside));
    }

    [Fact]
    public void CreateFolder_RollsBackDirectoryCreatedBeforeFailure()
    {
        var parent = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var fileSystem = new FaultInjectingFileSystem
        {
            CreateThenThrowOnCreateNumber = 1
        };

        var result = new ProjectContentFileOperations(fileSystem).CreateFolder(
            contentRoot,
            parent,
            "Birds");

        Assert.False(result.Succeeded);
        Assert.True(result.RollbackSucceeded);
        Assert.Empty(result.UnrestoredPaths);
        Assert.False(Directory.Exists(Path.Combine(parent, "Birds")));
    }

    [Fact]
    public void MoveFolder_MovesNestedTreeAndPreservesSourceSidecarColocation()
    {
        var source = Directory.CreateDirectory(Path.Combine(contentRoot, "Models", "Birds")).FullName;
        var nested = Directory.CreateDirectory(Path.Combine(source, "Nested")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(contentRoot, "Archive")).FullName;
        var group = WriteAssetGroup(nested, "Duck.glb");

        var result = new ProjectContentFileOperations().MoveFolder(contentRoot, source, destination);

        Assert.True(result.Succeeded, result.Error);
        var movedFolder = Path.Combine(destination, "Birds");
        var movedSource = Path.Combine(movedFolder, "Nested", "Duck.glb");
        Assert.False(Directory.Exists(source));
        Assert.True(File.Exists(movedSource));
        foreach (var metadataPath in group.AssetInfoPaths)
        {
            var movedMetadata = Path.Combine(movedFolder, "Nested", Path.GetFileName(metadataPath));
            Assert.True(AssetSourcePathContract.TryResolve(
                movedMetadata,
                ReadDeclaredFilename(movedMetadata),
                out var resolution,
                out var error), error);
            Assert.True(ProjectContentPathPolicy.IsSamePath(movedSource, resolution.SourcePath));
        }
    }

    [Fact]
    public void MoveFolder_RejectsDescendantAndCollisionWithoutMutation()
    {
        var source = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var descendant = Directory.CreateDirectory(Path.Combine(source, "Nested")).FullName;
        var collisionParent = Directory.CreateDirectory(Path.Combine(contentRoot, "Archive")).FullName;
        Directory.CreateDirectory(Path.Combine(collisionParent, "Models"));
        File.WriteAllText(Path.Combine(source, "marker.txt"), "source");

        var operations = new ProjectContentFileOperations();
        var descendantResult = operations.MoveFolder(contentRoot, source, descendant);
        var collisionResult = operations.MoveFolder(contentRoot, source, collisionParent);

        Assert.False(descendantResult.Succeeded);
        Assert.Contains("descendant", descendantResult.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(collisionResult.Succeeded);
        Assert.Contains("already exists", collisionResult.Error, StringComparison.OrdinalIgnoreCase);
        Assert.Equal("source", File.ReadAllText(Path.Combine(source, "marker.txt")));
    }

    [Fact]
    public void RenameFolder_RenamesTreeAndRejectsTraversalNames()
    {
        var source = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        WriteAssetGroup(source, "Duck.glb");
        var operations = new ProjectContentFileOperations();

        foreach (var invalidName in new[] { "..", "../Outside", "Nested/Outside", "Nested\\Outside", "CON" })
        {
            var invalidResult = operations.RenameFolder(contentRoot, source, invalidName);
            Assert.False(invalidResult.Succeeded);
            Assert.True(Directory.Exists(source));
        }

        var result = operations.RenameFolder(contentRoot, source, "BirdModels");

        Assert.True(result.Succeeded, result.Error);
        Assert.False(Directory.Exists(source));
        Assert.True(File.Exists(Path.Combine(contentRoot, "BirdModels", "Duck.glb")));
        Assert.True(File.Exists(Path.Combine(contentRoot, "BirdModels", "Duck.glb.asset")));
    }

    [Fact]
    public void DeleteFolder_DeletesNestedTree()
    {
        var source = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        WriteAssetGroup(source, "Duck.glb");

        var result = new ProjectContentFileOperations().DeleteFolder(contentRoot, source);

        Assert.True(result.Succeeded, result.Error);
        Assert.False(Directory.Exists(source));
        Assert.DoesNotContain(
            Directory.EnumerateDirectories(contentRoot),
            path => Path.GetFileName(path).StartsWith(".sailor-folder-", StringComparison.Ordinal));
    }

    [Fact]
    public void DeleteFolder_ReportsRollbackIncompleteAfterPartialRecursiveDelete()
    {
        var source = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        File.WriteAllText(Path.Combine(source, "first.txt"), "first");
        File.WriteAllText(Path.Combine(source, "second.txt"), "second");
        var fileSystem = new FaultInjectingFileSystem
        {
            DeleteOneFileThenThrowOnDeleteNumber = 1
        };

        var result = new ProjectContentFileOperations(fileSystem).DeleteFolder(contentRoot, source);

        Assert.False(result.Succeeded);
        Assert.False(result.RollbackSucceeded);
        Assert.Contains(result.UnrestoredPaths, path => ProjectContentPathPolicy.IsSamePath(path, source));
        Assert.True(Directory.Exists(source));
        Assert.Single(Directory.EnumerateFiles(source));
    }

    [Fact]
    public void FolderOperations_RejectContentRootOutsideRootAndNoOps()
    {
        var source = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var outside = Directory.CreateDirectory(Path.Combine(testRoot, "Outside")).FullName;
        var operations = new ProjectContentFileOperations();

        Assert.False(operations.RenameFolder(contentRoot, contentRoot, "RenamedContent").Succeeded);
        Assert.False(operations.DeleteFolder(contentRoot, contentRoot).Succeeded);
        Assert.False(operations.MoveFolder(contentRoot, contentRoot, outside).Succeeded);
        Assert.False(operations.MoveFolder(contentRoot, source, Path.GetDirectoryName(source)!).Succeeded);
        Assert.False(operations.RenameFolder(contentRoot, source, "Models").Succeeded);
        Assert.False(operations.MoveFolder(contentRoot, source, outside).Succeeded);
        Assert.True(Directory.Exists(source));
    }

    [Fact]
    public void AssetOperations_RejectNoOpsAndTraversalRename()
    {
        var sourceDirectory = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var group = WriteAssetGroup(sourceDirectory, "Duck.glb");
        var operations = new ProjectContentFileOperations();

        Assert.False(operations.MoveAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            sourceDirectory).Succeeded);
        Assert.False(operations.RenameAssetGroup(
            contentRoot,
            group.PrimaryAssetInfoPath,
            "Duck.glb").Succeeded);

        foreach (var invalidName in new[] { "..", "../Bird", "Nested/Bird", "Nested\\Bird", "NUL" })
        {
            Assert.False(operations.RenameAssetGroup(
                contentRoot,
                group.PrimaryAssetInfoPath,
                invalidName).Succeeded);
        }

        Assert.All(group.AllPaths, path => Assert.True(File.Exists(path)));
    }

    AssetGroupFixture WriteAssetGroup(string directory, string sourceFilename)
    {
        var sourcePath = WriteSource(directory, sourceFilename, "source:" + sourceFilename);
        var primary = WriteMetadata(
            directory,
            sourceFilename + ".asset",
            sourceFilename,
            "{PRIMARY-" + Guid.NewGuid().ToString("N") + "}");
        var secondaryTexture = WriteMetadata(
            directory,
            sourceFilename + "_BaseColor.asset",
            sourceFilename,
            "{TEXTURE-" + Guid.NewGuid().ToString("N") + "}");
        var secondaryProjection = WriteMetadata(
            directory,
            sourceFilename + "_BaseColor.png.asset",
            sourceFilename,
            "{PROJECTION-" + Guid.NewGuid().ToString("N") + "}");
        return new AssetGroupFixture(sourcePath, primary, [secondaryTexture, secondaryProjection]);
    }

    static string WriteSource(string directory, string filename, string contents)
    {
        var path = Path.Combine(directory, filename);
        File.WriteAllText(path, contents);
        return Path.GetFullPath(path);
    }

    static string WriteMetadata(
        string directory,
        string metadataFilename,
        string sourceFilename,
        string fileId)
    {
        var path = Path.Combine(directory, metadataFilename);
        File.WriteAllText(
            path,
            $"assetInfoType: Sailor::TextureAssetInfo{Environment.NewLine}" +
            $"fileId: \"{fileId}\"{Environment.NewLine}" +
            $"filename: {sourceFilename}{Environment.NewLine}" +
            $"marker: {metadataFilename}{Environment.NewLine}");
        return Path.GetFullPath(path);
    }

    static string ReadDeclaredFilename(string metadataPath)
        => ReadScalar(metadataPath, "filename");

    static string ReadFileId(string metadataPath)
        => ReadScalar(metadataPath, "fileId");

    static string ReadScalar(string metadataPath, string field)
    {
        var root = ReadMetadataRoot(metadataPath);
        var node = Assert.IsType<YamlScalarNode>(root.Children[new YamlScalarNode(field)]);
        return Assert.IsType<string>(node.Value);
    }

    static YamlMappingNode ReadMetadataRoot(string metadataPath)
    {
        var yaml = new YamlStream();
        using var reader = File.OpenText(metadataPath);
        yaml.Load(reader);
        return Assert.IsType<YamlMappingNode>(Assert.Single(yaml.Documents).RootNode);
    }

    static Dictionary<string, string> Snapshot(IEnumerable<string> paths)
        => paths.ToDictionary(
            Path.GetFullPath,
            File.ReadAllText,
            ProjectContentPathPolicy.PathComparer);

    static void AssertFilesEqual(IReadOnlyDictionary<string, string> expected)
    {
        foreach (var entry in expected)
        {
            Assert.True(File.Exists(entry.Key), $"Expected restored file: {entry.Key}");
            Assert.Equal(entry.Value, File.ReadAllText(entry.Key));
        }
    }

    static void CreateDirectoryLink(string linkPath, string targetPath)
    {
        if (!OperatingSystem.IsWindows())
        {
            Directory.CreateSymbolicLink(linkPath, targetPath);
            return;
        }

        var startInfo = new ProcessStartInfo("cmd.exe")
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("/d");
        startInfo.ArgumentList.Add("/c");
        startInfo.ArgumentList.Add("mklink");
        startInfo.ArgumentList.Add("/J");
        startInfo.ArgumentList.Add(linkPath);
        startInfo.ArgumentList.Add(targetPath);

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Could not create a directory junction.");
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Could not create a directory junction: {process.StandardError.ReadToEnd()}{process.StandardOutput.ReadToEnd()}");
        }
    }

    public void Dispose()
    {
        foreach (var linkName in new[] { "ExternalLink", "ExternalCreateLink", "ExternalDuplicateLink" })
        {
            var linkPath = Path.Combine(contentRoot, linkName);
            if (Directory.Exists(linkPath))
                Directory.Delete(linkPath);
        }

        if (Directory.Exists(testRoot))
            Directory.Delete(testRoot, recursive: true);
    }

    sealed record AssetGroupFixture(
        string SourcePath,
        string PrimaryAssetInfoPath,
        IReadOnlyList<string> SecondaryAssetInfoPaths)
    {
        public IReadOnlyList<string> AssetInfoPaths => [PrimaryAssetInfoPath, .. SecondaryAssetInfoPaths];
        public IReadOnlyList<string> AllPaths => [SourcePath, .. AssetInfoPaths];
    }

    sealed class FaultInjectingFileSystem : IProjectContentFileSystem
    {
        readonly IProjectContentFileSystem inner = PhysicalProjectContentFileSystem.Instance;
        int copyCount;
        int moveCount;
        int writeCount;
        int deleteFileCount;
        int deleteCount;
        int createCount;

        public int? ThrowOnCopyNumber { get; init; }
        public int? CopyThenThrowOnCopyNumber { get; init; }
        public int? ThrowOnMoveNumber { get; init; }
        public int? CopyThenThrowOnMoveNumber { get; init; }
        public int? ThrowOnWriteNumber { get; init; }
        public int? WriteThenThrowOnWriteNumber { get; init; }
        public int? ThrowOnDeleteFileNumber { get; init; }
        public int? DeleteOneFileThenThrowOnDeleteNumber { get; init; }
        public int? CreateThenThrowOnCreateNumber { get; init; }

        public bool FileExists(string path) => inner.FileExists(path);
        public bool DirectoryExists(string path) => inner.DirectoryExists(path);
        public IEnumerable<string> EnumerateFiles(string directoryPath) => inner.EnumerateFiles(directoryPath);
        public string ReadAllText(string path) => inner.ReadAllText(path);
        public void MoveDirectory(string sourcePath, string destinationPath) => inner.MoveDirectory(sourcePath, destinationPath);

        public void CopyFile(string sourcePath, string destinationPath)
        {
            copyCount++;
            if (copyCount == CopyThenThrowOnCopyNumber)
            {
                inner.CopyFile(sourcePath, destinationPath);
                throw new IOException($"Injected partial copy failure #{copyCount}.");
            }
            if (copyCount == ThrowOnCopyNumber)
                throw new IOException($"Injected copy failure #{copyCount}.");
            inner.CopyFile(sourcePath, destinationPath);
        }

        public void CreateDirectory(string path)
        {
            createCount++;
            inner.CreateDirectory(path);
            if (createCount == CreateThenThrowOnCreateNumber)
                throw new IOException($"Injected create failure #{createCount}.");
        }

        public void DeleteDirectory(string path, bool recursive)
        {
            deleteCount++;
            if (deleteCount == DeleteOneFileThenThrowOnDeleteNumber)
            {
                var file = Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories)
                    .OrderBy(candidate => candidate, StringComparer.Ordinal)
                    .First();
                File.Delete(file);
                throw new IOException($"Injected partial recursive delete failure #{deleteCount}.");
            }

            inner.DeleteDirectory(path, recursive);
        }

        public void MoveFile(string sourcePath, string destinationPath)
        {
            moveCount++;
            if (moveCount == CopyThenThrowOnMoveNumber)
            {
                File.Copy(sourcePath, destinationPath);
                throw new IOException($"Injected partial move failure #{moveCount}.");
            }
            if (moveCount == ThrowOnMoveNumber)
                throw new IOException($"Injected move failure #{moveCount}.");
            inner.MoveFile(sourcePath, destinationPath);
        }

        public void DeleteFile(string path)
        {
            deleteFileCount++;
            if (deleteFileCount == ThrowOnDeleteFileNumber)
                throw new IOException($"Injected file delete failure #{deleteFileCount}.");
            inner.DeleteFile(path);
        }

        public void WriteAllText(string path, string contents)
        {
            writeCount++;
            if (writeCount == ThrowOnWriteNumber)
                throw new IOException($"Injected write failure #{writeCount}.");
            inner.WriteAllText(path, contents);
            if (writeCount == WriteThenThrowOnWriteNumber)
                throw new IOException($"Injected partial write failure #{writeCount}.");
        }

        public void WriteAllTextNew(string path, string contents)
        {
            writeCount++;
            if (writeCount == ThrowOnWriteNumber)
                throw new IOException($"Injected write failure #{writeCount}.");
            inner.WriteAllTextNew(path, contents);
            if (writeCount == WriteThenThrowOnWriteNumber)
                throw new IOException($"Injected partial write failure #{writeCount}.");
        }
    }
}
