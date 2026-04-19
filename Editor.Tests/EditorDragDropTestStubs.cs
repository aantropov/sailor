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
    }
}

namespace SailorEditor.ViewModels
{
    public class AssetFile
    {
        public SailorEngine.FileId? FileId { get; set; }
    }

    public sealed class MaterialFile : AssetFile;
    public sealed class TextureFile : AssetFile;
    public sealed class PrefabFile : AssetFile;
    public sealed class AssetFolder;

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
    }

    public sealed class WorldState
    {
        public List<PrefabState> Prefabs { get; } = [];
    }

    public sealed class PrefabState
    {
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

    public sealed class InstantiatePrefabAssetCommand(SailorEditor.ViewModels.PrefabFile prefabFile, SailorEditor.ViewModels.GameObject? parent = null) : IEditorCommand
    {
        public string Name => nameof(InstantiatePrefabAssetCommand);
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
