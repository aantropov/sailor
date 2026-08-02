using Google.Protobuf;
using SailorEditor.Protocol.Generated;

namespace Editor.Tests;

public sealed class EditorEngineProtocolTests
{
    [Fact]
    public void RequestRoundTrip_PreservesVersionIdentityUnicodeAndCommand()
    {
        var request = new ProtocolRequest
        {
            ProtocolVersion = 1,
            RequestId = 42,
            UpdateObject = new UpdateObjectRequest
            {
                InstanceId = "объект-🦆",
                YamlChanges = "name: Утка\n"
            }
        };

        var parsed = ProtocolRequest.Parser.ParseFrom(request.ToByteArray());

        Assert.Equal(1u, parsed.ProtocolVersion);
        Assert.Equal(42ul, parsed.RequestId);
        Assert.Equal(ProtocolRequest.CommandOneofCase.UpdateObject, parsed.CommandCase);
        Assert.Equal("объект-🦆", parsed.UpdateObject.InstanceId);
        Assert.Equal("name: Утка\n", parsed.UpdateObject.YamlChanges);
    }

    [Fact]
    public void UpdateAssetRoundTrip_PreservesFileId()
    {
        var request = new ProtocolRequest
        {
            ProtocolVersion = 1,
            RequestId = 57,
            UpdateAsset = new FileIdRequest
            {
                FileId = "{01234567-89AB-CDEF-0123-456789ABCDEF}"
            }
        };

        var parsed = ProtocolRequest.Parser.ParseFrom(request.ToByteArray());

        Assert.Equal(ProtocolRequest.CommandOneofCase.UpdateAsset, parsed.CommandCase);
        Assert.Equal(
            "{01234567-89AB-CDEF-0123-456789ABCDEF}",
            parsed.UpdateAsset.FileId);
    }

    [Fact]
    public void ResponseRoundTrip_PreservesTypedViewportEvent()
    {
        var response = new ProtocolResponse
        {
            ProtocolVersion = 1,
            RequestId = 99,
            Success = true,
            ViewportEventBatchResult = new ViewportEventBatchResult()
        };
        response.ViewportEventBatchResult.Events.Add(new ViewportEvent
        {
            Revision = 8,
            ManagedMutationRevision = 5,
            Transform = new ViewportTransformEvent
            {
                InstanceId = "game-object",
                Operation = ViewportTransformOperation.Rotate,
                Space = ViewportTransformSpace.Local,
                BeforeRotation = new Vector4 { W = 1.0f },
                AfterRotation = new Vector4 { Y = 0.70710677f, W = 0.70710677f }
            }
        });

        var parsed = ProtocolResponse.Parser.ParseFrom(response.ToByteArray());
        var viewportEvent = Assert.Single(parsed.ViewportEventBatchResult.Events);

        Assert.Equal(ProtocolResponse.ResultOneofCase.ViewportEventBatchResult, parsed.ResultCase);
        Assert.Equal(ViewportEvent.PayloadOneofCase.Transform, viewportEvent.PayloadCase);
        Assert.Equal(ViewportTransformOperation.Rotate, viewportEvent.Transform.Operation);
        Assert.Equal(ViewportTransformSpace.Local, viewportEvent.Transform.Space);
        Assert.Equal(0.70710677f, viewportEvent.Transform.AfterRotation.Y);
    }

    [Fact]
    public void NewReader_IgnoresUnknownAdditiveFields()
    {
        var request = new ProtocolRequest
        {
            ProtocolVersion = 1,
            RequestId = 7,
            GetExitCode = new Empty()
        };
        var knownBytes = request.ToByteArray();
        var bytesWithUnknownField = new byte[knownBytes.Length + 3];
        knownBytes.CopyTo(bytesWithUnknownField, 0);
        bytesWithUnknownField[^3] = 0xA0;
        bytesWithUnknownField[^2] = 0x06;
        bytesWithUnknownField[^1] = 0x01;

        var parsed = ProtocolRequest.Parser.ParseFrom(bytesWithUnknownField);

        Assert.Equal(ProtocolRequest.CommandOneofCase.GetExitCode, parsed.CommandCase);
        Assert.Equal(7ul, parsed.RequestId);
    }

    [Fact]
    public void CommandAndResultFieldNumbers_AreStableForVersionOne()
    {
        Assert.Equal(10, ProtocolRequest.InitializeFieldNumber);
        Assert.Equal(31, ProtocolRequest.SendRemoteViewportInputFieldNumber);
        Assert.Equal(43, ProtocolRequest.SetEditorSelectionFieldNumber);
        Assert.Equal(45, ProtocolRequest.RenderPathTracedImageFieldNumber);
        Assert.Equal(46, ProtocolRequest.SerializeEngineTypesFieldNumber);
        Assert.Equal(
            47,
            ProtocolRequest.IsEngineMainThreadReadyFieldNumber);
        Assert.Equal(48, ProtocolRequest.IsEngineRunningFieldNumber);
        Assert.Equal(57, ProtocolRequest.UpdateAssetFieldNumber);
        Assert.Equal(10, ProtocolResponse.EmptyResultFieldNumber);
        Assert.Equal(19, ProtocolResponse.ViewportEventBatchResultFieldNumber);
    }

    [Fact]
    public void GeneratedSources_LiveInTheRequiredLanguageFolders()
    {
        var root = RepositoryRoot();

        Assert.True(File.Exists(Path.Combine(root, "Protocol", "editor_engine.proto")));
        Assert.True(File.Exists(Path.Combine(root, "Runtime", "Protocol", "Generated", "editor_engine.pb.h")));
        Assert.True(File.Exists(Path.Combine(root, "Runtime", "Protocol", "Generated", "editor_engine.pb.cc")));
        Assert.True(File.Exists(Path.Combine(root, "Editor", "Protocol", "Generated", "EditorEngine.cs")));
    }

    [Fact]
    public void Builds_VerifyTemporaryGenerationWithoutOverwritingCheckedInSources()
    {
        var root = RepositoryRoot();
        var cmake = File.ReadAllText(Path.Combine(root, "Lib", "CMakeLists.txt"));
        var project = File.ReadAllText(
            Path.Combine(root, "Editor", "Protocol", "SailorEditor.Protocol.csproj"));

        Assert.Contains("SailorProtocolGeneratedSourcesCheck", cmake, StringComparison.Ordinal);
        Assert.Contains("CMAKE_CURRENT_BINARY_DIR", cmake, StringComparison.Ordinal);
        Assert.Contains("-E compare_files", cmake, StringComparison.Ordinal);
        Assert.Contains(
            "add_dependencies(SailorLib SailorProtocolGeneratedSourcesCheck)",
            cmake,
            StringComparison.Ordinal);
        Assert.Contains("SAILOR_PROTOCOL_GENERATED_CPP_SOURCES", cmake, StringComparison.Ordinal);
        Assert.DoesNotContain("PROPERTIES GENERATED TRUE", cmake, StringComparison.Ordinal);
        Assert.DoesNotContain(
            "\"--cpp_out=${SAILOR_PROTOCOL_GENERATED_CPP_DIR}\"",
            cmake,
            StringComparison.Ordinal);

        Assert.Contains(
            "/obj/$(Configuration)/$(TargetFramework)/ProtocolGenerated/",
            project,
            StringComparison.Ordinal);
        Assert.Contains("CompileOutputs=\"false\"", project, StringComparison.Ordinal);
        Assert.Contains("<GetFileHash", project, StringComparison.Ordinal);
        Assert.Contains("Algorithm=\"SHA256\"", project, StringComparison.Ordinal);
        Assert.Contains("Checked-in C# protocol source", project, StringComparison.Ordinal);
        Assert.DoesNotContain("OutputDir=\"Generated\"", project, StringComparison.Ordinal);
    }

    static string RepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null &&
               !File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt")))
        {
            directory = directory.Parent;
        }

        return directory?.FullName
            ?? throw new DirectoryNotFoundException("Could not locate the Sailor repository root.");
    }
}
