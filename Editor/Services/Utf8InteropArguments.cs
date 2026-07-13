#nullable enable

using System.Runtime.InteropServices;

namespace SailorEngine;

internal static class Utf8InteropArguments
{
    public static nint[] Allocate(string[] arguments, int count)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        if (count != arguments.Length)
        {
            throw new ArgumentOutOfRangeException(
                nameof(count),
                count,
                "The native argument count must match the managed argument array length.");
        }

        var nativeArguments = new nint[count];
        try
        {
            for (var index = 0; index < count; ++index)
            {
                var argument = arguments[index]
                    ?? throw new ArgumentException(
                        $"Command-line argument {index} must not be null.",
                        nameof(arguments));
                if (argument.Contains('\0', StringComparison.Ordinal))
                {
                    throw new ArgumentException(
                        $"Command-line argument {index} must not contain an embedded null character.",
                        nameof(arguments));
                }

                nativeArguments[index] = Marshal.StringToCoTaskMemUTF8(argument);
            }

            return nativeArguments;
        }
        catch
        {
            Free(nativeArguments);
            throw;
        }
    }

    public static void Free(nint[] nativeArguments)
    {
        ArgumentNullException.ThrowIfNull(nativeArguments);
        for (var index = 0; index < nativeArguments.Length; ++index)
        {
            if (nativeArguments[index] == nint.Zero)
                continue;

            Marshal.FreeCoTaskMem(nativeArguments[index]);
            nativeArguments[index] = nint.Zero;
        }
    }
}
