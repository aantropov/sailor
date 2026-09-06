using System.Runtime.InteropServices;

namespace SailorEditor.Protocol;

internal static class EngineProtocolNative
{
#if MACCATALYST
    // Keep one engine handle for the editor process, including runtime restarts.
    static readonly Lazy<nint> bundledEngine = new(LoadBundledEngine);

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

                return bundledEngine.Value;
            });
    }

    static nint LoadBundledEngine()
    {
        var bundledPath = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory, "..", "Resources", $"{EngineLibrary}.dylib"));
        return MacEngineLibraryLoader.Load(bundledPath);
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
        EntryPoint = "SailorProtocolStartLocalHost",
        ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SailorProtocolStartLocalHost(
        [In] byte[] initializeRequestData,
        uint initializeRequestSize,
        ushort port,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string authorizationToken,
        uint authorizationTokenSize);

    [DllImport(
        EngineLibrary,
        EntryPoint = "SailorProtocolRequestLocalHostStop",
        ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SailorProtocolRequestLocalHostStop();

    [DllImport(
        EngineLibrary,
        EntryPoint = "SailorProtocolStopLocalHost",
        ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SailorProtocolStopLocalHost(
        [MarshalAs(UnmanagedType.I1)] bool shutdownEngine);

#if WINDOWS
    [DllImport(
        EngineLibrary,
        EntryPoint = "SailorProtocolSetWindowsViewportHost",
        ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SailorProtocolSetWindowsViewportHost(
        ulong viewportId,
        nint swapChainPanelInspectable,
        float compositionScale);
#endif
}
