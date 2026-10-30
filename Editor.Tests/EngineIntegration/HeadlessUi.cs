using System.Collections.Concurrent;

namespace Editor.Tests.EngineIntegration
{
    // UI notifications stay queued so tests can deliver them after a generation changes.
    static class MainThread
    {
        static readonly ConcurrentQueue<Action> pending = new();

        public static bool IsMainThread => true;
        public static bool QueueInvocations { get; set; }
        public static void BeginInvokeOnMainThread(Action action) => pending.Enqueue(action);
        public static Task InvokeOnMainThreadAsync(Func<Task> action) => action();
        public static Task<T> InvokeOnMainThreadAsync<T>(Func<Task<T>> action) => action();
        public static Task<T> InvokeOnMainThreadAsync<T>(Func<T> action) => Task.FromResult(action());
        public static Task InvokeOnMainThreadAsync(Action action)
        {
            if (QueueInvocations)
            {
                var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                pending.Enqueue(() =>
                {
                    try
                    {
                        action();
                        completion.SetResult();
                    }
                    catch (Exception error)
                    {
                        completion.SetException(error);
                    }
                });
                return completion.Task;
            }
            action();
            return Task.CompletedTask;
        }

        public static void Drain()
        {
            while (DispatchNext())
            {
            }
        }

        public static bool DispatchNext()
        {
            if (!pending.TryDequeue(out var action))
            {
                return false;
            }
            action();
            return true;
        }
    }

    readonly record struct Rect(double X, double Y, double Width, double Height)
    {
        public bool IsEmpty => Width <= 0 || Height <= 0;
    }
}

namespace Microsoft.Maui.Storage
{
    sealed class FileSystem
    {
        public static FileSystem Current { get; } = new();
        public string AppDataDirectory => Path.GetTempPath();
    }
}

namespace SailorEditor
{
    static class MauiProgram
    {
        static readonly ConcurrentDictionary<Type, object> services = new();
        public static void SetService<T>(T service) where T : class => services[typeof(T)] = service;
        public static T GetService<T>() => (T)services[typeof(T)];
        public static void ClearServices() => services.Clear();
    }
}

namespace SailorEditor.Services
{
    public sealed class WorldService
    {
        public Dictionary<string, SailorEditor.ViewModels.GameObject> GameObjects { get; } = new();
        public Dictionary<string, SailorEditor.ViewModels.Component> Components { get; } = new();
        public event Action<object?>? OnUpdateWorldAction;
        public void PublishUpdate() => OnUpdateWorldAction?.Invoke(null);
        public IEnumerable<SailorEditor.ViewModels.Component> GetComponents(SailorEditor.ViewModels.GameObject gameObject)
            => Components.Values.Where(component => component.Owner == gameObject);
        public long WorkspaceEpoch { get; set; }
        public bool TryPopulateWorld(string yaml, long epoch) => throw new NotSupportedException();
        public object CreatePrefabFromSubHierarchy(SailorEditor.ViewModels.GameObject root,
            out List<SailorEngine.InstanceId> externalReferences) => throw new NotSupportedException();
        public bool TryGetGameObject(SailorEngine.InstanceId id,
            out SailorEditor.ViewModels.GameObject gameObject) => GameObjects.TryGetValue(id.Value, out gameObject!);
        public bool TryGetComponent(SailorEngine.InstanceId id,
            out SailorEditor.ViewModels.Component component) => Components.TryGetValue(id.Value, out component!);
        public SailorEditor.ViewModels.GameObject FindOwner(SailorEditor.ViewModels.Component component)
            => component.Owner!;
        public Func<SailorEditor.ViewModels.GameObject, string, CancellationToken, Task<bool>> AddComponentOperation { get; set; } = (_, _, _) => Task.FromResult(true);
        public Func<SailorEditor.ViewModels.Component, CancellationToken, Task<bool>> ResetComponentOperation { get; set; } = (_, _) => Task.FromResult(true);
        public Func<SailorEditor.ViewModels.Component, CancellationToken, Task<bool>> RemoveComponentOperation { get; set; } = (_, _) => Task.FromResult(true);
        public Task<bool> AddComponentAsync(SailorEditor.ViewModels.GameObject gameObject, string typeName, CancellationToken token = default) => AddComponentOperation(gameObject, typeName, token);
        public Task<bool> ResetComponentToDefaultsAsync(SailorEditor.ViewModels.Component component, CancellationToken token = default) => ResetComponentOperation(component, token);
        public Task<bool> RemoveComponentAsync(SailorEditor.ViewModels.Component component, CancellationToken token = default) => RemoveComponentOperation(component, token);
    }

    public sealed class ComponentClipboardService
    {
        public Func<SailorEditor.ViewModels.Component, CancellationToken, Task> PasteOperation { get; set; } = (_, _) => Task.CompletedTask;
        public Task PasteValuesAsync(SailorEditor.ViewModels.Component component, CancellationToken token = default) => PasteOperation(component, token);
    }
}

namespace SailorEditor.ViewModels
{
    // Refresh and selection use the real AssetFile; scene objects supply only projection data.
    public sealed class ModelFile : AssetFile
    {
        public Task<bool> LoadDependentResources(CancellationToken token)
            => base.LoadDependentResources().WaitAsync(token);
    }
    public sealed class TextureFile : AssetFile
    {
        public string Filtration { get; set; } = "Linear";
        public string Format { get; set; } = "R8G8B8A8_SRGB";
        public string Clamping { get; set; } = "Repeat";
        public bool ShouldGenerateMips { get; set; }
        public bool ShouldSupportStorageBinding { get; set; }
        public bool ShouldKeepCpuBuffers { get; set; }
        public string SamplerReduction { get; set; } = "Average";
    }
    public sealed class MaterialFile : AssetFile;
    public sealed class ShaderFile : AssetFile;
    public sealed class AnimationFile : AssetFile;
    public sealed class AnimationControllerFile : AssetFile;
    public sealed class AnimationSetFile : AssetFile;
    public sealed class AudioFile : AssetFile;
    public sealed class PrefabFile : AssetFile;
    public sealed class WorldFile : AssetFile;
    public sealed class World : AssetFile;
    public sealed class ShaderLibraryFile : AssetFile;
    public sealed class FrameGraphFile : AssetFile;
    public sealed class LandscapeVegetationFile : AssetFile;
    public sealed class GIProbesFile : AssetFile;
    public sealed class GameObject : CommunityToolkit.Mvvm.ComponentModel.ObservableObject, IInspectorEditable
    {
        public string Name { get; set; } = "Test object";
        public SailorEngine.InstanceId InstanceId { get; set; } = new("test-object");
        public Vec4 Position { get; set; } = new();
        public Rotation Rotation { get; set; } = new();
        public Vec4 Scale { get; set; } = new();
        public string MobilityType { get; set; } = "Stationary";
        public Func<CancellationToken, Task<bool>> Commit { get; set; } = _ => Task.FromResult(true);
        public bool HasPendingInspectorChanges { get; private set; }
        public bool HasInFlightInspectorCommit => false;
        public async Task<bool> CommitInspectorChangesAsync(CancellationToken cancellationToken = default)
        {
            var succeeded = await Commit(cancellationToken);
            if (succeeded)
            {
                HasPendingInspectorChanges = false;
            }
            return succeeded;
        }
    }
    public sealed class Component : CommunityToolkit.Mvvm.ComponentModel.ObservableObject, IInspectorEditable
    {
        public SailorEngine.InstanceId InstanceId { get; set; } = new("test-component");
        public string DisplayName { get; set; } = "Test component";
        public SailorEngine.ComponentType? Typename { get; set; }
        public GameObject? Owner { get; set; }
        public bool IsUndefined { get; set; }
        public SailorEditor.Utility.ObservableDictionary<string, CommunityToolkit.Mvvm.ComponentModel.ObservableObject> OverrideProperties { get; } = [];
        public Func<CancellationToken, Task<bool>> Commit { get; set; } = _ => Task.FromResult(true);
        public bool HasPendingInspectorChanges { get; private set; }
        public bool HasInFlightInspectorCommit => false;
        public async Task<bool> CommitInspectorChangesAsync(CancellationToken cancellationToken = default)
        {
            var succeeded = await Commit(cancellationToken);
            if (succeeded)
            {
                HasPendingInspectorChanges = false;
            }
            return succeeded;
        }
        public Task<bool> ApplyInspectorBatchAsync(Action mutation, CancellationToken cancellationToken = default)
        {
            mutation();
            HasPendingInspectorChanges = true;
            return CommitInspectorChangesAsync(cancellationToken);
        }
    }
}

namespace SailorEditor.Views
{
    public static class AddComponentDialogPage
    {
        public static Func<IEnumerable<string>, Task<string?>> Choose { get; set; } = _ => Task.FromResult<string?>(null);
        public static Task<string?> ShowAsync(IEnumerable<string> names) => Choose(names);
    }
}

namespace SailorEditor.Commands
{
    static class EditorYaml
    {
        public static string SerializePrefab(object prefab) => throw new NotSupportedException();
    }
}
