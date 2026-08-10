using SailorEditor.Commands;

namespace SailorEngine
{
    public sealed class FileId(string value = "")
    {
        public string Value { get; set; } = value;
        public bool IsEmpty() => string.IsNullOrWhiteSpace(Value);
        public override bool Equals(object? obj) => obj is FileId other && string.Equals(Value, other.Value, StringComparison.Ordinal);
        public override int GetHashCode() => Value.GetHashCode(StringComparison.Ordinal);
    }

    public sealed class InstanceId(string value = "")
    {
        public string Value { get; set; } = value;
        public bool IsEmpty() => string.IsNullOrWhiteSpace(Value);
    }

    public sealed class Vec4
    {
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
        public float W { get; set; }
    }
}

namespace SailorEditor.ViewModels
{
    public class AssetFile
    {
        internal const string DefaultAssetInfoTypeName = "Sailor::AssetInfo";

        public SailorEngine.FileId? FileId { get; set; }
        public string AssetInfoTypeName { get; set; } = string.Empty;
        public FileInfo? Asset { get; set; }
        public bool IsReadOnly { get; set; }
    }

    public sealed class AudioFile : AssetFile
    {
    }

    public sealed class MaterialFile : AssetFile;
    public sealed class TextureFile : AssetFile;
    public sealed class ModelFile : AssetFile;
    public sealed class AnimationControllerFile : AssetFile;
    public sealed class AnimationSetFile : AssetFile;
    public sealed class PrefabFile : AssetFile;
    public sealed class AssetFolder
    {
        public int Id { get; set; }
        public bool IsReadOnly { get; set; }
    }

    public sealed class GameObject
    {
        public SailorEngine.InstanceId? InstanceId { get; set; }
        public uint ParentIndex { get; set; } = uint.MaxValue;
        public int PrefabIndex { get; set; }
    }
}

namespace SailorEditor.Services
{
    public sealed class WorldService
    {
        public WorldState Current { get; } = new();

        public bool TryGetGameObject(SailorEngine.InstanceId? instanceId, out SailorEditor.ViewModels.GameObject? gameObject)
        {
            gameObject = Current.Prefabs
                .SelectMany(prefab => prefab.GameObjects)
                .FirstOrDefault(go => go.InstanceId is not null && instanceId is not null && string.Equals(go.InstanceId.Value, instanceId.Value, StringComparison.Ordinal));
            return gameObject is not null;
        }

        public SailorEngine.InstanceId? ResolveParentInstanceId(
            SailorEditor.ViewModels.GameObject gameObject)
        {
            if (gameObject.PrefabIndex < 0 ||
                gameObject.PrefabIndex >= Current.Prefabs.Count)
            {
                return null;
            }

            var prefab = Current.Prefabs[gameObject.PrefabIndex];
            if (gameObject.ParentIndex != uint.MaxValue)
            {
                if (gameObject.ParentIndex >= prefab.GameObjects.Count)
                {
                    return null;
                }

                return prefab.GameObjects[(int)gameObject.ParentIndex]
                    .InstanceId;
            }

            return string.IsNullOrWhiteSpace(prefab.ParentInstanceId)
                ? null
                : new SailorEngine.InstanceId(prefab.ParentInstanceId);
        }
    }

    public sealed class WorldState
    {
        public List<PrefabState> Prefabs { get; } = [];
    }

    public sealed class PrefabState
    {
        public string? ParentInstanceId { get; set; }
        public List<SailorEditor.ViewModels.GameObject> GameObjects { get; } = [];
    }
}

namespace SailorEditor.Commands
{
    public sealed class CreatePrefabAssetCommand(SailorEditor.ViewModels.GameObject gameObject, SailorEditor.ViewModels.AssetFolder? targetFolder = null, SailorEditor.ViewModels.PrefabFile? existingPrefab = null) : IEditorCommand
    {
        public string Name => nameof(CreatePrefabAssetCommand);
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
    }

    public sealed class InstantiatePrefabAssetCommand(
        SailorEditor.ViewModels.AssetFile prefabFile,
        SailorEditor.ViewModels.GameObject? parent = null,
        SailorEngine.Vec4? worldPosition = null) : IUndoableEditorCommand
    {
        public string Name => nameof(InstantiatePrefabAssetCommand);
        public string Description => "Instantiate Prefab";
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default) => ValueTask.FromResult(CommandResult.Success());
    }

    public sealed class CreateModelGameObjectCommand(
        SailorEditor.ViewModels.AssetFile modelFile,
        string objectName,
        SailorEditor.ViewModels.GameObject? parent = null,
        SailorEngine.Vec4? worldPosition = null) : IUndoableEditorCommand
    {
        public string Name => nameof(CreateModelGameObjectCommand);
        public string Description => $"Create {objectName}";
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default) => ValueTask.FromResult(CommandResult.Success());
    }

    public sealed class MoveAssetCommand(SailorEditor.ViewModels.AssetFile assetFile, SailorEditor.ViewModels.AssetFolder? destinationFolder = null) : IEditorCommand
    {
        public string Name => nameof(MoveAssetCommand);
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
    }

    public sealed class MoveFolderCommand(SailorEditor.ViewModels.AssetFolder folder, SailorEditor.ViewModels.AssetFolder? destinationFolder = null) : IEditorCommand
    {
        public string Name => nameof(MoveFolderCommand);
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
    }

    public sealed class ReparentGameObjectCommand(SailorEditor.ViewModels.GameObject child, SailorEditor.ViewModels.GameObject? newParent, bool keepWorldTransform = true) : IEditorCommand
    {
        public string Name => nameof(ReparentGameObjectCommand);
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
    }
}

namespace SailorEditor
{
    public static class MauiProgram
    {
        static readonly IServiceProvider Services = new ServiceProviderStub();
        public static TService GetService<TService>() => (TService)Services.GetService(typeof(TService))!;

        sealed class ServiceProviderStub : IServiceProvider
        {
            readonly SailorEditor.Services.WorldService worldService = new();
            public object? GetService(Type serviceType) => serviceType == typeof(SailorEditor.Services.WorldService) ? worldService : null;
        }
    }
}
