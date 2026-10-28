using System.Reflection;
using Google.Protobuf;
using SailorEditor.Protocol;
using SailorEditor.Protocol.Generated;
using SailorEditor.Services;
using SailorEditor.Workspace;
using SailorEngine;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class EngineAssetPreviewInvalidationTests
{
    [Theory]
    [InlineData(false, true, false, 1)]
    [InlineData(true, true, false, 1)]
    [InlineData(false, false, false, 0)]
    [InlineData(true, false, false, 0)]
    [InlineData(false, true, true, 0)]
    [InlineData(true, true, true, 0)]
    public async Task TargetedUpdate_InvalidatesPreviewsOnlyAfterSuccessInCurrentEngineGeneration(
        bool reimport, bool success, bool engineReplaced, int expectedNotifications)
    {
        var temporary = Directory.CreateTempSubdirectory("sailor-preview-invalidation-");
        try
        {
            var serializer = new WorkspaceManifestSerializer();
            var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
                new RecentWorkspaceStore(Path.Combine(temporary.FullName, "Recent.yaml")));
            EngineService? engine = null;
            var requests = 0;
            using var client = new EngineProtocolClient((bytes, _, token) =>
            {
                token.ThrowIfCancellationRequested();
                var request = ProtocolRequest.Parser.ParseFrom(bytes);
                Assert.Equal(ProtocolRequest.CommandOneofCase.UpdateAsset, request.CommandCase);
                Assert.Equal(reimport, request.UpdateAsset.Reimport);
                requests++;
                if (engineReplaced)
                {
                    SetField(engine!, "engineGeneration", 2L);
                }
                return Task.FromResult(new ProtocolResponse
                {
                    ProtocolVersion = request.ProtocolVersion,
                    RequestId = request.RequestId,
                    Success = true,
                    BoolResult = new BoolResult { Value = success }
                }.ToByteArray());
            });
            await using (engine = new EngineService(lifecycle, client))
            {
                // The transport fixture replaces process startup; calls still use the production service gate.
                SetField(engine, "lifecycleState", (int)EngineLifecycleState.Running);
                SetField(engine, "engineGeneration", 1L);
                var notifications = 0;
                engine.OnAssetUpdated += () => notifications++;
                try
                {
                    var id = new FileId("{89ABCDEF-0123-4567-89AB-CDEF01234567}");
                    var result = reimport ? await engine.ReimportAssetAsync(id) : await engine.UpdateAssetAsync(id);
                    Assert.Equal(success, result);
                    Assert.Equal(1, requests);
                    Assert.Equal(expectedNotifications, notifications);
                }
                finally
                {
                    SetField(engine, "lifecycleState", (int)EngineLifecycleState.Stopped);
                }
            }
        }
        finally
        {
            temporary.Delete(true);
        }
    }

    static void SetField(EngineService engine, string name, object value) =>
        typeof(EngineService).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(engine, value);
}
