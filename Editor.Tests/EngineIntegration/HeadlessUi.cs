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
        public long WorkspaceEpoch => throw new NotSupportedException();
        public bool TryPopulateWorld(string yaml, long epoch) => throw new NotSupportedException();
        public object CreatePrefabFromSubHierarchy(SailorEditor.ViewModels.GameObject root,
            out List<SailorEngine.InstanceId> externalReferences) => throw new NotSupportedException();
        public bool TryGetGameObject(SailorEngine.InstanceId id,
            out SailorEditor.ViewModels.GameObject gameObject) => GameObjects.TryGetValue(id.Value, out gameObject!);
        public bool TryGetComponent(SailorEngine.InstanceId id,
            out SailorEditor.ViewModels.Component component) => Components.TryGetValue(id.Value, out component!);
        public SailorEditor.ViewModels.GameObject FindOwner(SailorEditor.ViewModels.Component component)
            => component.Owner!;
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
    public sealed class TextureFile : AssetFile;
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
    public sealed class GameObject : CommunityToolkit.Mvvm.ComponentModel.ObservableObject
    {
        public string Name { get; set; } = "Test object";
        public SailorEngine.InstanceId InstanceId { get; set; } = new("test-object");
    }
    public sealed class Component : CommunityToolkit.Mvvm.ComponentModel.ObservableObject
    {
        public SailorEngine.InstanceId InstanceId { get; set; } = new("test-component");
        public string DisplayName { get; set; } = "Test component";
        public SailorEngine.ComponentType? Typename { get; set; }
        public GameObject? Owner { get; set; }
    }
}

namespace SailorEditor.Commands
{
    static class EditorYaml
    {
        public static string SerializePrefab(object prefab) => throw new NotSupportedException();
    }
}
