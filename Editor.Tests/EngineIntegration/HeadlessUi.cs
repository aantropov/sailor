using System.Collections.Concurrent;

namespace Editor.Tests.EngineIntegration
{
    // UI notifications stay queued so tests can deliver them after a generation changes.
    static class MainThread
    {
        static readonly ConcurrentQueue<Action> pending = new();

        public static bool IsMainThread => true;
        public static void BeginInvokeOnMainThread(Action action) => pending.Enqueue(action);
        public static Task InvokeOnMainThreadAsync(Func<Task> action) => action();
        public static Task<T> InvokeOnMainThreadAsync<T>(Func<Task<T>> action) => action();
        public static Task InvokeOnMainThreadAsync(Action action)
        {
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
        public static T GetService<T>() => throw new NotSupportedException("This test does not create UI services.");
    }
}

namespace SailorEditor.Services
{
    sealed class WorldService
    {
        public long WorkspaceEpoch => throw new NotSupportedException();
        public bool TryPopulateWorld(string yaml, long epoch) => throw new NotSupportedException();
    }
}

namespace SailorEditor.ViewModels
{
    sealed class ModelFile;
    sealed class TextureFile;
    sealed class MaterialFile;
    sealed class ShaderFile;
    sealed class AnimationFile;
    sealed class AnimationControllerFile;
    sealed class AnimationSetFile;
    sealed class AudioFile;
}
