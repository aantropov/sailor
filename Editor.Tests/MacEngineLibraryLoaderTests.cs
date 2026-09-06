using System.Runtime.InteropServices;
using SailorEditor.Protocol;
using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class MacEngineLibraryLoaderTests
{
    [Fact]
    public async Task Load_MakesWeakEngineSymbolsAvailableToReloadedWorkspaceModules()
    {
        if (!OperatingSystem.IsMacOS())
            return;

        var root = Directory.CreateTempSubdirectory("sailor-dyld-tests-").FullName;
        nint localEngine = nint.Zero;
        nint globalEngine = nint.Zero;
        try
        {
            // Unique C++ symbols keep this fixture independent of other images
            // that dyld may retain after dlclose because of weak coalescing.
            var symbolNamespace = "Engine_" + Guid.NewGuid().ToString("N");
            var engineSource = Path.Combine(root, "Engine.cpp");
            var moduleSource = Path.Combine(root, "Workspace.cpp");
            var enginePath = Path.Combine(root, "libEngine.dylib");
            var modulePath = Path.Combine(root, "libWorkspace.dylib");
            await File.WriteAllTextAsync(engineSource, $$"""
                namespace {{symbolNamespace}}
                {
                    template<typename T> struct Registration { static int value; };
                    template<typename T> int Registration<T>::value = 41;
                    template struct Registration<int>;
                }
                extern "C" int ReadEngineRegistration()
                {
                    return {{symbolNamespace}}::Registration<int>::value;
                }
                """);
            await File.WriteAllTextAsync(moduleSource, $$"""
                namespace {{symbolNamespace}}
                {
                    template<typename T> struct Registration { static int value; };
                    extern template struct Registration<int>;
                }
                extern "C" int ReadWorkspaceRegistration()
                {
                    return {{symbolNamespace}}::Registration<int>::value + 1;
                }
                """);

            await CompileAsync(root, "clang++", "-std=c++20", "-dynamiclib", engineSource,
                "-o", enginePath, "-Wl,-install_name," + enginePath);
            await CompileAsync(root, "clang++", "-std=c++20", "-dynamiclib", moduleSource,
                enginePath, "-o", modulePath);

            // Match Mono's local host load and DynamicLibrary's local module
            // load explicitly; CoreCLR's NativeLibrary flags differ from Mono.
            localEngine = LoadLocal(enginePath);
            Assert.NotEqual(nint.Zero, localEngine);
            var localModule = LoadLocal(modulePath);
            try
            {
                Assert.Equal(nint.Zero, localModule);
            }
            finally
            {
                if (localModule != nint.Zero)
                    NativeLibrary.Free(localModule);
            }

            globalEngine = MacEngineLibraryLoader.Load(enginePath);
            for (var restart = 0; restart < 3; ++restart)
            {
                var module = LoadLocal(modulePath);
                Assert.NotEqual(nint.Zero, module);
                try
                {
                    var read = Marshal.GetDelegateForFunctionPointer<ReadRegistration>(
                        NativeLibrary.GetExport(module, "ReadWorkspaceRegistration"));
                    Assert.Equal(42, read());
                }
                finally
                {
                    NativeLibrary.Free(module);
                }
            }
        }
        finally
        {
            if (globalEngine != nint.Zero)
                NativeLibrary.Free(globalEngine);
            if (localEngine != nint.Zero)
                NativeLibrary.Free(localEngine);
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Load_MissingLibraryReportsItsPath()
    {
        if (!OperatingSystem.IsMacOS())
            return;

        var path = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".dylib");
        var error = Assert.Throws<DllNotFoundException>(() => MacEngineLibraryLoader.Load(path));
        Assert.Contains(path, error.Message);
    }

    static async Task CompileAsync(string directory, params string[] arguments)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var result = await new WorkspaceProcessRunner().RunAsync(
            new WorkspaceProcessInvocation("/usr/bin/xcrun", arguments, directory), timeout.Token);
        Assert.True(result.Succeeded, result.Output);
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int ReadRegistration();

    static nint LoadLocal(string path) => Dlopen(path, 0x2 | 0x4);

    [DllImport("/usr/lib/libSystem.B.dylib", EntryPoint = "dlopen", CallingConvention = CallingConvention.Cdecl)]
    static extern nint Dlopen([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int mode);
}
