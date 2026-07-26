using System.Runtime.InteropServices;

namespace SailorEditor.Protocol;

internal static class EngineProtocolNative
{
#if MACCATALYST
    static EngineProtocolNative()
    {
        NativeLibrary.SetDllImportResolver(
            typeof(EngineProtocolNative).Assembly,
            static (libraryName, assembly, searchPath) =>
            {
                if (libraryName != EngineLibrary)
                {
                    return nint.Zero;
                }

                var bundledPath = Path.Combine(
                    AppContext.BaseDirectory,
                    "..",
                    "Resources",
                    $"{EngineLibrary}.dylib");
                bundledPath = Path.GetFullPath(bundledPath);

                return File.Exists(bundledPath)
                    ? NativeLibrary.Load(bundledPath)
                    : nint.Zero;
            });
    }
#endif

#if MACCATALYST
#if DEBUG
    const string EngineLibrary = "Sailor-Debug";
#else
    const string EngineLibrary = "Sailor-Release";
#endif
#elif DEBUG
    const string EngineLibrary = "../../../../../Sailor-RelWithDebInfo.dll";
#else
    const string EngineLibrary = "../../../../../Sailor-Release.dll";
#endif

    [DllImport(
        EngineLibrary,
        EntryPoint = "SailorProtocolInvoke",
        ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SailorProtocolInvoke(
        [In] byte[] requestData,
        uint requestSize,
        out nint responseData,
        out uint responseSize);

    [DllImport(
        EngineLibrary,
        EntryPoint = "SailorProtocolFreeBuffer",
        ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SailorProtocolFreeBuffer(nint buffer);
}
