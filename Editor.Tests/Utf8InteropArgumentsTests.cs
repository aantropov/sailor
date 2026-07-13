using System.Runtime.InteropServices;
using System.Text;
using SailorEngine;

namespace Editor.Tests;

public sealed class Utf8InteropArgumentsTests
{
    [Fact]
    public void Allocate_PreservesUtf8ArgumentsAndTokenBoundaries()
    {
        string[] arguments =
        [
            "SailorEditor",
            "--workspace",
            Path.Combine(Path.GetTempPath(), "Physical-РАБОЧАЯ-AZ with spaces"),
            "--workspace-manifest",
            "Проект с пробелами.sailor"
        ];
        var nativeArguments = Utf8InteropArguments.Allocate(arguments, arguments.Length);

        try
        {
            Assert.Equal(arguments.Length, nativeArguments.Length);
            for (var index = 0; index < arguments.Length; ++index)
            {
                Assert.NotEqual(nint.Zero, nativeArguments[index]);
                Assert.Equal(arguments[index], Marshal.PtrToStringUTF8(nativeArguments[index]));
                Assert.Equal(
                    0,
                    Marshal.ReadByte(
                        nativeArguments[index],
                        Encoding.UTF8.GetByteCount(arguments[index])));
            }
        }
        finally
        {
            Utf8InteropArguments.Free(nativeArguments);
        }

        Assert.All(nativeArguments, pointer => Assert.Equal(nint.Zero, pointer));
        Utf8InteropArguments.Free(nativeArguments);
    }

    [Fact]
    public void Allocate_RejectsInvalidCountAndEmbeddedNull()
    {
        Assert.Throws<ArgumentOutOfRangeException>(
            () => Utf8InteropArguments.Allocate(["SailorEditor"], 0));
        Assert.Throws<ArgumentException>(
            () => Utf8InteropArguments.Allocate(
                ["SailorEditor", "workspace\0suffix"],
                2));
    }
}
