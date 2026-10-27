using Google.Protobuf;
using SailorEditor.Protocol;
using SailorEditor.Protocol.Generated;
using SailorEditor.Services;

namespace Editor.Tests;

public sealed class MaterialPreviewProtocolTests
{
    [Fact]
    public async Task Update_SendsActualUnsavedSnapshotAndCamera_WithoutWaitingForGpu()
    {
        ProtocolRequest? sent = null;
        using var client = Create(request =>
        {
            sent = request;
            return new() { SessionId = 12, Revision = 7, State = MaterialPreviewState.Pending };
        });
        const string yaml = "shaderUid: custom\ndefines: [ALPHA_CUTOUT]\nuniformsFloat: {roughness: 0.3}";
        var result = await client.UpdateMaterialPreviewAsync(new(12, 7,
            new(yaml, PreviewPrimitive.Plane, 0.8f, -0.2f, 5, 512, 512)));

        Assert.Equal(PreviewState.Pending, result.State);
        Assert.NotNull(sent?.UpdateMaterialPreview);
        Assert.Equal(yaml, sent.UpdateMaterialPreview.MaterialYaml);
        Assert.Equal(MaterialPreviewPrimitive.Plane, sent.UpdateMaterialPreview.Primitive);
        Assert.Equal(0.8f, sent.UpdateMaterialPreview.Yaw);
        Assert.Equal(-0.2f, sent.UpdateMaterialPreview.Pitch);
        Assert.Equal(5, sent.UpdateMaterialPreview.Distance);
        Assert.Equal(512u, sent.UpdateMaterialPreview.Width);
        Assert.Equal(7ul, sent.UpdateMaterialPreview.Revision);
    }

    [Fact]
    public async Task Get_RequestsOnlyANewerImage_AndCloseUsesSameSession()
    {
        var sent = new List<ProtocolRequest>();
        using var client = Create(request =>
        {
            sent.Add(request);
            return request.GetMaterialPreview is not null
                ? new() { SessionId = 42, Revision = 4, State = MaterialPreviewState.Ready, Width = 384, Height = 384, Png = ByteString.CopyFrom([1, 2, 3]) }
                : new() { SessionId = 42, State = MaterialPreviewState.Closed };
        });
        var result = await client.GetMaterialPreviewAsync(42, 3);
        await client.CloseMaterialPreviewAsync(42);
        Assert.Equal(new byte[] { 1, 2, 3 }, result.Png);
        Assert.Equal(3ul, sent[0].GetMaterialPreview.AfterRevision);
        Assert.Equal(42ul, sent[0].GetMaterialPreview.SessionId);
        Assert.Equal(42ul, sent[1].CloseMaterialPreview.SessionId);
    }

    [Theory]
    [InlineData(0ul, 1ul, 1, 384u, 384u, 0f)]
    [InlineData(1ul, 0ul, 1, 384u, 384u, 0f)]
    [InlineData(1ul, 1ul, 0, 384u, 384u, 0f)]
    [InlineData(1ul, 1ul, 4, 384u, 384u, 0f)]
    [InlineData(1ul, 1ul, 1, 513u, 384u, 0f)]
    [InlineData(1ul, 1ul, 1, 384u, 1u, 0f)]
    [InlineData(1ul, 1ul, 1, 384u, 384u, float.NaN)]
    public async Task InvalidInput_DoesNotReachNative(ulong session, ulong revision, int primitive, uint width, uint height, float yaw)
    {
        using var client = Create(_ => throw new InvalidOperationException("Invalid input must not be sent."));
        await Assert.ThrowsAnyAsync<ArgumentException>(() => client.UpdateMaterialPreviewAsync(
            new(session, revision, new("shaderUid: test", (PreviewPrimitive)primitive, yaw, Width: width, Height: height))));
    }

    [Fact]
    public async Task OversizedYamlAndOversizedResult_AreRejected()
    {
        using var client = Create(_ => new() { SessionId = 1, Revision = 1, State = MaterialPreviewState.Ready, Width = 4096 });
        await Assert.ThrowsAsync<ArgumentException>(() => client.UpdateMaterialPreviewAsync(new(1, 1, new(new string('x', 1024 * 1024 + 1)))));
        await Assert.ThrowsAsync<EngineProtocolException>(() => client.GetMaterialPreviewAsync(1, 0));
    }

    static EngineProtocolClient Create(Func<ProtocolRequest, MaterialPreviewResult> result) => new((bytes, _, cancellation) =>
    {
        cancellation.ThrowIfCancellationRequested();
        var request = ProtocolRequest.Parser.ParseFrom(bytes);
        return Task.FromResult(new ProtocolResponse
        {
            ProtocolVersion = request.ProtocolVersion,
            RequestId = request.RequestId,
            Success = true,
            MaterialPreviewResult = result(request)
        }.ToByteArray());
    });
}
