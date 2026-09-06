using System.Runtime.InteropServices;

namespace SailorEditor.Protocol;

internal static class MacEngineLibraryLoader
{
    const int RtldNow = 0x2;
    const int RtldGlobal = 0x8;

    public static nint Load(string path)
    {
        if (!OperatingSystem.IsMacOS() && !OperatingSystem.IsMacCatalyst())
            throw new PlatformNotSupportedException("The engine library loader requires macOS.");
        ArgumentException.ThrowIfNullOrWhiteSpace(path);

        // Mono's DllImportResolver only recognizes handles registered through
        // NativeLibrary. A raw dlopen handle would fall back to a second image.
        var handle = NativeLibrary.Load(path);
        try
        {
            // Promote this same image so dyld can bind weak C++ template exports
            // in workspace modules, without changing the modules' local scope.
            _ = Dlerror();
            var globalHandle = Dlopen(path, RtldNow | RtldGlobal);
            if (globalHandle == nint.Zero)
            {
                var error = Marshal.PtrToStringUTF8(Dlerror()) ?? "dlopen returned a null handle.";
                throw new DllNotFoundException($"Cannot expose engine library '{path}': {error}");
            }
            // NativeLibrary owns the remaining reference for the editor lifetime.
            Dlclose(globalHandle);
            return handle;
        }
        catch
        {
            NativeLibrary.Free(handle);
            throw;
        }
    }

    [DllImport("/usr/lib/libSystem.B.dylib", EntryPoint = "dlopen", CallingConvention = CallingConvention.Cdecl)]
    static extern nint Dlopen([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int mode);

    [DllImport("/usr/lib/libSystem.B.dylib", EntryPoint = "dlerror", CallingConvention = CallingConvention.Cdecl)]
    static extern nint Dlerror();

    [DllImport("/usr/lib/libSystem.B.dylib", EntryPoint = "dlclose", CallingConvention = CallingConvention.Cdecl)]
    static extern int Dlclose(nint handle);
}
