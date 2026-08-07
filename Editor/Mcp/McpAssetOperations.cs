#nullable enable

using SailorEditor.AI;
using SailorEditor.Commands;
using SailorEditor.Content;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Mcp;

internal sealed class McpAssetOperations
{
    readonly AssetsService _assets;
    readonly ICommandDispatcher _dispatcher;
    readonly IActionContextProvider _contextProvider;
    readonly AIOperatorService _aiOperator;

    public McpAssetOperations(
        AssetsService assets,
        ICommandDispatcher dispatcher,
        IActionContextProvider contextProvider,
        AIOperatorService aiOperator)
    {
        _assets = assets;
        _dispatcher = dispatcher;
        _contextProvider = contextProvider;
        _aiOperator = aiOperator;
    }

    public async Task<IReadOnlyList<McpAssetSnapshot>> ListAsync(
        string? path,
        string? type,
        bool recursive,
        CancellationToken cancellationToken = default)
    {
        var directories = ResolveListDirectoryPaths(path);
        foreach (var directory in directories)
        {
            var rootFolder = await _assets.ResolveFolderAsync(
                directory,
                cancellationToken);
            if (rootFolder is null)
                continue;
            if (recursive)
                await EnsureFolderTreeLoadedAsync(rootFolder, cancellationToken);
            else
                await _assets.EnsureFolderLoadedAsync(rootFolder.Id, cancellationToken);
        }

        return _assets.Assets.Values
            .Where(asset => directories.Any(directory =>
                MatchesDirectory(asset, directory, recursive)))
            .Where(asset => string.IsNullOrWhiteSpace(type) || MatchesType(asset, type))
            .OrderBy(asset => GetSourcePath(asset), PathComparer)
            .Select(asset => BuildSnapshot(asset, includeProperties: false))
            .ToArray();
    }

    IReadOnlyList<string> ResolveListDirectoryPaths(string? path)
    {
        if (Path.IsPathRooted(path))
            return [Path.GetFullPath(path)];

        var relativePath = path?.Trim() ?? string.Empty;
        var contentRoots = _assets.Folders
            .Where(folder => folder.ParentFolderId == -1)
            .Select(folder => folder.FullPath)
            .Append(_assets.CurrentProjectRootPath)
            .Where(root => !string.IsNullOrWhiteSpace(root))
            .Distinct(PathComparer)
            .ToArray();
        return contentRoots
            .Select(root => string.IsNullOrEmpty(relativePath)
                ? Path.GetFullPath(root)
                : Path.GetFullPath(relativePath, root))
            .Distinct(PathComparer)
            .ToArray();
    }

    public async Task<McpAssetSnapshot?> GetAsync(
        string target,
        CancellationToken cancellationToken = default)
    {
        var asset = await ResolveAssetAsync(target, cancellationToken);
        if (asset is null)
            return null;

        await asset.EnsureMetadataLoadedAsync(cancellationToken);
        return BuildSnapshot(asset, includeProperties: true);
    }

    public async Task<McpAssetMutationResult> ExecuteAsync(
        bool confirm,
        string kind,
        string? target,
        string? destinationFolder,
        string? newName,
        string? folderPath,
        string? folderName,
        CancellationToken cancellationToken = default)
    {
        if (!confirm)
        {
            return new McpAssetMutationResult(
                false,
                kind,
                null,
                "Asset mutations require confirm=true.");
        }

        var normalizedKind = kind.Trim().ToLowerInvariant();
        IEditorCommand? command;
        string? currentFileId = null;
        switch (normalizedKind)
        {
            case "rename_asset":
            case "delete_asset":
            case "duplicate_asset":
            case "move_asset":
            case "reimport_asset":
            {
                var asset = await ResolveAssetAsync(target, cancellationToken);
                if (asset is null)
                    return Failure(kind, $"Asset '{target}' was not found.");
                currentFileId = asset.FileId?.Value;
                var destination = normalizedKind == "move_asset"
                    ? await ResolveFolderAsync(destinationFolder, cancellationToken)
                    : null;
                if (normalizedKind == "move_asset" &&
                    !string.IsNullOrWhiteSpace(destinationFolder) &&
                    destination is null)
                {
                    return Failure(kind, $"Destination folder '{destinationFolder}' was not found.");
                }

                command = normalizedKind switch
                {
                    "rename_asset" when !string.IsNullOrWhiteSpace(newName) =>
                        new RenameAssetCommand(asset, newName),
                    "delete_asset" => new DeleteAssetCommand(asset),
                    "duplicate_asset" => new DuplicateAssetCommand(asset),
                    "move_asset" => new MoveAssetCommand(asset, destination),
                    "reimport_asset" => new ReimportAssetCommand(asset),
                    _ => null,
                };
                break;
            }
            case "create_folder":
            {
                var parent = await ResolveFolderAsync(folderPath, cancellationToken);
                if (!string.IsNullOrWhiteSpace(folderPath) && parent is null &&
                    !ProjectContentPathPolicy.IsSamePath(
                        ResolveDirectoryPath(folderPath),
                        _assets.CurrentProjectRootPath))
                {
                    return Failure(kind, $"Parent folder '{folderPath}' was not found.");
                }
                command = !string.IsNullOrWhiteSpace(folderName)
                    ? new CreateFolderCommand(parent, folderName)
                    : null;
                break;
            }
            case "rename_folder":
            case "delete_folder":
            case "move_folder":
            {
                var folder = await ResolveFolderAsync(folderPath, cancellationToken);
                if (folder is null)
                    return Failure(kind, $"Folder '{folderPath}' was not found.");
                var destination = normalizedKind == "move_folder"
                    ? await ResolveFolderAsync(destinationFolder, cancellationToken)
                    : null;
                if (normalizedKind == "move_folder" &&
                    !string.IsNullOrWhiteSpace(destinationFolder) &&
                    destination is null)
                {
                    return Failure(kind, $"Destination folder '{destinationFolder}' was not found.");
                }

                command = normalizedKind switch
                {
                    "rename_folder" when !string.IsNullOrWhiteSpace(newName) =>
                        new RenameFolderCommand(folder, newName),
                    "delete_folder" => new DeleteFolderCommand(folder),
                    "move_folder" => new MoveFolderCommand(folder, destination),
                    _ => null,
                };
                break;
            }
            default:
                return Failure(kind, $"Unknown asset operation '{kind}'.");
        }

        if (command is null)
            return Failure(kind, "The asset operation arguments are incomplete.");

        var context = _contextProvider.GetCurrentContext(
            new CommandOrigin(CommandOriginKind.AI, "MCP", "External MCP Agent"),
            new Dictionary<string, string?>
            {
                ["mcp"] = "true",
                ["mcpAssetOperation"] = normalizedKind,
            });
        var result = await _dispatcher.DispatchAsync(
            command,
            context,
            cancellationToken);
        var resultFileId = result.Value switch
        {
            FileId fileId => fileId.Value,
            string value => value,
            _ => currentFileId,
        };
        _aiOperator.RecordExternalExecution(
            $"MCP asset {normalizedKind}",
            AIActionSafety.ConfirmRequired,
            result.Succeeded ? AIProposalState.Executed : AIProposalState.Failed,
            [new AIActionExecutionItem(command.Name, result.Succeeded, result.Message)],
            result.Message);
        return new McpAssetMutationResult(
            result.Succeeded,
            normalizedKind ?? kind,
            resultFileId,
            result.Message ?? (result.Succeeded
                ? "Asset operation completed."
                : "Asset operation failed."));
    }

    async Task<AssetFile?> ResolveAssetAsync(
        string? target,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(target))
            return null;

        var normalized = target.Trim();
        var byFileId = await _assets.ResolveAssetAsync(
            new FileId(normalized),
            cancellationToken);
        if (byFileId is not null)
            return byFileId;

        foreach (var fullPath in ResolveFilePaths(normalized))
        {
            var directory = Path.GetDirectoryName(fullPath);
            if (!string.IsNullOrWhiteSpace(directory))
                await _assets.ResolveFolderAsync(directory, cancellationToken);
            var asset = _assets.Assets.Values.FirstOrDefault(candidate =>
                IsSamePath(candidate.AssetInfo?.FullName, fullPath) ||
                IsSamePath(candidate.Asset?.FullName, fullPath));
            if (asset is not null)
                return asset;
        }

        return null;
    }

    Task<AssetFolder?> ResolveFolderAsync(
        string? path,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(path))
            return Task.FromResult<AssetFolder?>(null);
        var fullPath = ResolveDirectoryPath(path);
        return _assets.ResolveFolderAsync(fullPath, cancellationToken);
    }

    async Task EnsureFolderTreeLoadedAsync(
        AssetFolder rootFolder,
        CancellationToken cancellationToken)
    {
        var pending = new Queue<AssetFolder>();
        pending.Enqueue(rootFolder);
        while (pending.Count > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var folder = pending.Dequeue();
            await _assets.EnsureFolderLoadedAsync(folder.Id, cancellationToken);
            foreach (var child in _assets.Folders.Where(candidate =>
                         candidate.ParentFolderId == folder.Id))
            {
                pending.Enqueue(child);
            }
        }
    }

    McpAssetSnapshot BuildSnapshot(
        AssetFile asset,
        bool includeProperties)
    {
        IReadOnlyDictionary<string, object?>? properties = null;
        string? yaml = null;
        if (includeProperties && asset.AssetInfo?.Exists == true)
        {
            yaml = File.ReadAllText(asset.AssetInfo.FullName);
            properties = McpYamlUtilities.ToPlainObject(
                McpYamlUtilities.ParseMapping(yaml)) as
                IReadOnlyDictionary<string, object?>;
        }

        return new McpAssetSnapshot(
            asset.FileId?.Value ?? string.Empty,
            asset.DisplayName ?? asset.Asset?.Name ?? asset.AssetInfo?.Name ?? string.Empty,
            asset.AssetType?.Name ?? asset.GetType().Name,
            asset.AssetInfoTypeName ?? string.Empty,
            GetSourcePath(asset),
            asset.AssetInfo?.FullName ?? string.Empty,
            asset.IsReadOnly,
            asset.IsMetadataLoaded,
            properties,
            yaml);
    }

    string ResolveDirectoryPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return Path.GetFullPath(_assets.CurrentProjectRootPath);
        return Path.IsPathRooted(path)
            ? Path.GetFullPath(path)
            : Path.GetFullPath(path, _assets.CurrentProjectRootPath);
    }

    IReadOnlyList<string> ResolveFilePaths(string path)
    {
        if (Path.IsPathRooted(path))
            return [Path.GetFullPath(path)];

        return _assets.Folders
            .Where(folder => folder.ParentFolderId == -1)
            .Select(folder => folder.FullPath)
            .Prepend(_assets.CurrentProjectRootPath)
            .Where(root => !string.IsNullOrWhiteSpace(root))
            .Distinct(PathComparer)
            .Select(root => Path.GetFullPath(path, root))
            .ToArray();
    }

    static bool MatchesDirectory(
        AssetFile asset,
        string directory,
        bool recursive)
    {
        var assetDirectory = Path.GetDirectoryName(GetSourcePath(asset));
        if (string.IsNullOrWhiteSpace(assetDirectory))
            return false;
        if (!recursive)
            return IsSamePath(assetDirectory, directory);
        return ProjectContentPathPolicy.IsInsideRoot(directory, assetDirectory);
    }

    static bool MatchesType(AssetFile asset, string type)
    {
        return string.Equals(asset.AssetType?.Name, type, StringComparison.OrdinalIgnoreCase) ||
            string.Equals(asset.AssetInfoTypeName, type, StringComparison.OrdinalIgnoreCase) ||
            string.Equals(asset.GetType().Name, type, StringComparison.OrdinalIgnoreCase);
    }

    static string GetSourcePath(AssetFile asset) =>
        asset.Asset?.FullName ?? asset.AssetInfo?.FullName ?? string.Empty;

    static bool IsSamePath(string? left, string? right)
    {
        if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right))
            return false;
        return ProjectContentPathPolicy.IsSamePath(left, right);
    }

    static StringComparer PathComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    static McpAssetMutationResult Failure(string kind, string message) =>
        new(false, kind, null, message);
}
