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
            while (pending.TryDequeue(out var action)) action();
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
    sealed class WorldService
    {
        public long WorkspaceEpoch => throw new NotSupportedException();
        public bool TryPopulateWorld(string yaml, long epoch) => throw new NotSupportedException();
        public object CreatePrefabFromSubHierarchy(SailorEditor.ViewModels.GameObject root,
            out List<SailorEngine.InstanceId> externalReferences) => throw new NotSupportedException();
    }

    sealed class SelectionService
    {
        public SailorEditor.Workflow.SelectionSnapshot Snapshot => throw new NotSupportedException();
        public object SelectedItem => throw new NotSupportedException();
        public void SelectObject(object item, bool force = false) => throw new NotSupportedException();
        public void ClearSelection() => throw new NotSupportedException();
    }
}

namespace SailorEditor.ViewModels
{
    // Refresh uses the real AssetFile; inspector and scene surfaces stay outside this target.
    public sealed class ModelFile : AssetFile;
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
    public sealed class GameObject
    {
        public string Name => throw new NotSupportedException();
    }
}

namespace SailorEditor.Commands
{
    static class EditorYaml
    {
        public static string SerializePrefab(object prefab) => throw new NotSupportedException();
    }
}
