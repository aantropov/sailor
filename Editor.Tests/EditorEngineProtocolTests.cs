using Google.Protobuf;
using SailorEditor.Protocol.Generated;

namespace Editor.Tests;

public sealed class EditorEngineProtocolTests
{
    [Fact]
    public void GoldenExitCodeRequestAndResponse_MatchGeneratedMessages()
    {
        var requestBytes = Convert.FromHexString("0801109601820100");
        var request = ProtocolRequest.Parser.ParseFrom(requestBytes);
        Assert.Equal(1u, request.ProtocolVersion);
        Assert.Equal(150ul, request.RequestId);
        Assert.Equal(ProtocolRequest.CommandOneofCase.GetExitCode, request.CommandCase);
        Assert.Equal(requestBytes, request.ToByteArray());

        var responseBytes = Convert.FromHexString("0801109601180128016200");
        var response = ProtocolResponse.Parser.ParseFrom(responseBytes);
        Assert.Equal(1u, response.ProtocolVersion);
        Assert.Equal(150ul, response.RequestId);
        Assert.True(response.Success);
        Assert.True(response.SupportsStrictInstanceIds);
        Assert.Equal(ProtocolResponse.ResultOneofCase.Int32Result, response.ResultCase);
        Assert.Equal(0, response.Int32Result.Value);
        Assert.Equal(responseBytes, response.ToByteArray());
    }

    [Theory]
    [InlineData("5200", ProtocolResponse.ResultOneofCase.EmptyResult)]
    [InlineData("5A00", ProtocolResponse.ResultOneofCase.BoolResult)]
    [InlineData("6200", ProtocolResponse.ResultOneofCase.Int32Result)]
    [InlineData("6A00", ProtocolResponse.ResultOneofCase.Uint32Result)]
    [InlineData("7200", ProtocolResponse.ResultOneofCase.Uint64Result)]
    [InlineData("7A00", ProtocolResponse.ResultOneofCase.StringResult)]
    [InlineData("820100", ProtocolResponse.ResultOneofCase.StringListResult)]
    [InlineData("8A0100", ProtocolResponse.ResultOneofCase.AssetReloadStateResult)]
    [InlineData("920100", ProtocolResponse.ResultOneofCase.InstanceIdResult)]
    [InlineData("9A0100", ProtocolResponse.ResultOneofCase.ViewportEventBatchResult)]
    [InlineData("A20100", ProtocolResponse.ResultOneofCase.Vector4Result)]
    [InlineData("AA0100", ProtocolResponse.ResultOneofCase.ViewportToolStateResult)]
    [InlineData("B20100", ProtocolResponse.ResultOneofCase.AnimatorStateResult)]
    [InlineData("BA0100", ProtocolResponse.ResultOneofCase.EditorRenderModeResult)]
    [InlineData("C20100", ProtocolResponse.ResultOneofCase.GiProbesBakeStatusResult)]
    [InlineData("CA0100", ProtocolResponse.ResultOneofCase.GlobalIlluminationStateResult)]
    [InlineData("A20600", ProtocolResponse.ResultOneofCase.ModelFingerprintStatusResult)]
    public void GoldenEmptyResults_PreserveOneofPresence(string hex, ProtocolResponse.ResultOneofCase expected)
    {
        var bytes = Convert.FromHexString(hex);
        var response = ProtocolResponse.Parser.ParseFrom(bytes);
        Assert.Equal(expected, response.ResultCase);
        Assert.Equal(bytes, response.ToByteArray());
    }

    [Fact]
    public void GoldenScalars_PreserveBooleanAndNegativeInt32()
    {
        var boolBytes = Convert.FromHexString("0801109601180128015A020801");
        var boolean = ProtocolResponse.Parser.ParseFrom(boolBytes);
        Assert.Equal(ProtocolResponse.ResultOneofCase.BoolResult, boolean.ResultCase);
        Assert.True(boolean.BoolResult.Value);
        Assert.Equal(boolBytes, boolean.ToByteArray());

        var intBytes = Convert.FromHexString("080110960118012801620B08FDFFFFFFFFFFFFFFFF01");
        var integer = ProtocolResponse.Parser.ParseFrom(intBytes);
        Assert.Equal(ProtocolResponse.ResultOneofCase.Int32Result, integer.ResultCase);
        Assert.Equal(-3, integer.Int32Result.Value);
        Assert.Equal(intBytes, integer.ToByteArray());
    }

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

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void UpdateAssetRoundTrip_PreservesFileIdAndReimport(bool reimport)
    {
        var request = new ProtocolRequest
        {
            ProtocolVersion = 1,
            RequestId = 57,
            UpdateAsset = new UpdateAssetRequest
            {
                FileId = "{01234567-89AB-CDEF-0123-456789ABCDEF}",
                Reimport = reimport
            }
        };

        var parsed = ProtocolRequest.Parser.ParseFrom(request.ToByteArray());

        Assert.Equal(ProtocolRequest.CommandOneofCase.UpdateAsset, parsed.CommandCase);
        Assert.Equal(reimport, parsed.UpdateAsset.Reimport);
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
        Assert.Equal(61, ProtocolRequest.SetEditorSimulationFieldNumber);
        Assert.Equal(62, ProtocolRequest.GetEditorSimulationStateFieldNumber);
        Assert.Equal(64, ProtocolRequest.SetEditorStatsModeFieldNumber);
        Assert.Equal(65, ProtocolRequest.SetEditorRenderModeFieldNumber);
        Assert.Equal(66, ProtocolRequest.GetEditorRenderModeFieldNumber);
        Assert.Equal(70, ProtocolRequest.SetGiSettingsFieldNumber);
        Assert.Equal(71, ProtocolRequest.GetGlobalIlluminationStateFieldNumber);
        Assert.Equal(72, ProtocolRequest.SetRuntimeGiProbesPreviewFieldNumber);
        Assert.Equal(73, ProtocolRequest.SetRuntimeGiProbesPausedFieldNumber);
        Assert.Equal(74, ProtocolRequest.RestartRuntimeGiProbesFieldNumber);
        Assert.Equal(75, ProtocolRequest.RebuildRuntimeGiProbesSceneFieldNumber);
        Assert.Equal(
            76,
            ProtocolRequest.SetRuntimeGiProbesPreviewBudgetFieldNumber);
        Assert.Equal(10, ProtocolResponse.EmptyResultFieldNumber);
        Assert.Equal(19, ProtocolResponse.ViewportEventBatchResultFieldNumber);
        Assert.Equal(
            23,
            ProtocolResponse.EditorRenderModeResultFieldNumber);
        Assert.Equal(5, (int)EditorRenderMode.GlobalIlluminationOnly);
        Assert.Equal(12, (int)EditorRenderMode.GlobalIlluminationFallback);
        Assert.Equal(
            13,
            (int)EditorRenderMode.GlobalIlluminationSubdivisions);
        Assert.Equal(1, (int)GlobalIlluminationMode.NoGi);
        Assert.Equal(2, (int)GlobalIlluminationMode.Runtime);
        Assert.Equal(3, (int)GlobalIlluminationMode.Baked);
        Assert.Equal(2, SetGISettingsRequest.ModeFieldNumber);
        Assert.Equal(3, SetGISettingsRequest.RuntimeProbesFieldNumber);
        Assert.Equal(6, GlobalIlluminationStateResult.ModeFieldNumber);
        Assert.Equal(7, GlobalIlluminationStateResult.EnabledFieldNumber);
        Assert.Equal(8, GlobalIlluminationStateResult.RuntimeProbesFieldNumber);
        Assert.Equal(9, GlobalIlluminationStateResult.RuntimeStateFieldNumber);
        Assert.Equal(16, RuntimeGIProbesState.PreviewBudgetFieldNumber);
        Assert.Equal(1, (int)RuntimeGIProbesPreviewBudget.Eco);
        Assert.Equal(2, (int)RuntimeGIProbesPreviewBudget.Balanced);
    }

}
