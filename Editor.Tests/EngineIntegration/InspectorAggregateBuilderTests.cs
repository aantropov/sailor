using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workspace;
using SailorEngine;
using SailorEditor.Commands;
using SailorEditor.History;
using Component = SailorEditor.ViewModels.Component;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class InspectorAggregateBuilderTests
{
    [Fact]
    public void GameObjects_ShowOnlyCommonComponentsAndTheirEditableProperties()
    {
        var world = new WorldService();
        var first = AddGameObject(world, "first");
        var second = AddGameObject(world, "second");
        var light = new ComponentType { Name = "Test::Light", Properties = new() { ["intensity"] = new FloatProperty() } };
        AddComponent(world, first, "first-light", light, ("intensity", new Observable<float>(2f)));
        AddComponent(world, second, "second-light", light, ("intensity", new Observable<float>(2f)));
        AddComponent(world, first, "first-camera", new ComponentType { Name = "Test::Camera" });

        var model = new InspectorAggregateBuilder(world, null!, new EngineTypes()).Build([first, second]);

        Assert.Equal(["Object", "Transform", "Light", "Components"], model.Sections.Select(section => section.Name));
        var intensity = Assert.Single(model.Sections.Single(section => section.Name == "Light").Fields);
        Assert.Equal("intensity", intensity.Name);
        Assert.Equal("2", intensity.DisplayValue);
        Assert.False(model.HasPendingInspectorChanges);
    }

    [Fact]
    public async Task AxisEdit_PreservesSiblingAxes_AndResolvesTargetsReplacedDuringCommit()
    {
        var world = new WorldService();
        var first = AddGameObject(world, "first");
        var second = AddGameObject(world, "second");
        first.Position = new Vec4 { X = 1, Y = 10, Z = 20 };
        second.Position = new Vec4 { X = 2, Y = 30, Z = 40 };
        var replacement = new GameObject
        {
            Name = "Replacement",
            InstanceId = second.InstanceId,
            Position = new Vec4 { X = 3, Y = 300, Z = 400 }
        };
        first.Commit = _ =>
        {
            world.GameObjects[second.InstanceId.Value] = replacement;
            return Task.FromResult(true);
        };
        var model = new InspectorAggregateBuilder(world, null!, new EngineTypes()).Build([first, second]);
        var position = model.Sections.Single(section => section.Name == "Transform").Fields.Single(field => field.Name == "Position");
        var x = position.Children.Single(field => field.Name == "X");
        Assert.Equal("*", x.DisplayValue);
        x.SetInputText("1");

        Assert.True(await model.CommitInspectorChangesAsync());

        Assert.Equal(1f, first.Position.X);
        Assert.Equal(10f, first.Position.Y);
        Assert.Equal(20f, first.Position.Z);
        Assert.Equal(1f, replacement.Position.X);
        Assert.Equal(300f, replacement.Position.Y);
        Assert.Equal(400f, replacement.Position.Z);
        Assert.Equal(2f, second.Position.X);
        Assert.Equal("first", first.Name);
    }

    [Fact]
    public async Task NestedRecordEdit_ClonesEachPropertyAndPreservesOtherPerTargetValues()
    {
        var world = new WorldService();
        var owner = AddGameObject(world, "owner");
        var recordType = new ComponentType
        {
            Name = "Test::Settings",
            Properties = new() { ["strength"] = new FloatProperty(), ["label"] = new Property<string>() }
        };
        var type = new ComponentType
        {
            Name = "Test::Effect",
            Properties = new() { ["settings"] = new RecordProperty { RecordType = recordType, Typename = recordType.Name } }
        };
        var firstRecord = Record(2f, "first-label");
        var secondRecord = Record(3f, "second-label");
        var first = AddComponent(world, owner, "first", type, ("settings", firstRecord));
        var second = AddComponent(world, owner, "second", type, ("settings", secondRecord));
        var model = new InspectorAggregateBuilder(world, null!, new EngineTypes()).Build([first, second]);
        var settings = Assert.Single(Assert.Single(model.Sections).Fields);
        Assert.True(settings.IsMixed);
        settings.Children.Single(field => field.Name == "strength").SetDraft(9f);

        Assert.True(await model.CommitInspectorChangesAsync());

        var firstResult = (ObservableRecord)first.OverrideProperties["settings"];
        var secondResult = (ObservableRecord)second.OverrideProperties["settings"];
        Assert.Equal(9f, ((Observable<float>)firstResult.Fields["strength"]).Value);
        Assert.Equal(9f, ((Observable<float>)secondResult.Fields["strength"]).Value);
        Assert.Equal("first-label", ((Observable<string>)firstResult.Fields["label"]).Value);
        Assert.Equal("second-label", ((Observable<string>)secondResult.Fields["label"]).Value);
        Assert.Equal(2f, ((Observable<float>)firstRecord.Fields["strength"]).Value);
        Assert.Equal(3f, ((Observable<float>)secondRecord.Fields["strength"]).Value);
        Assert.NotSame(firstResult, secondResult);
    }

    [Fact]
    public async Task UnequalLists_ShowMixedValue_AndEditOnlySharedIndices()
    {
        var world = new WorldService();
        var owner = AddGameObject(world, "owner");
        var type = new ComponentType { Name = "Test::Weights", Properties = new() { ["weights"] = new Property<List<float>>() } };
        var first = AddComponent(world, owner, "first", type, ("weights", new ObservableFloatList([1f, 10f])));
        var second = AddComponent(world, owner, "second", type, ("weights", new ObservableFloatList([2f, 20f, 30f])));
        var model = new InspectorAggregateBuilder(world, null!, new EngineTypes()).Build([first, second]);
        var weights = Assert.Single(Assert.Single(model.Sections).Fields);
        Assert.Equal("*", weights.DisplayValue);
        Assert.Equal(2, weights.Children.Count);
        weights.Children[0].SetDraft(5f);

        Assert.True(await model.CommitInspectorChangesAsync());

        Assert.Equal([5f, 10f], ((ObservableFloatList)first.OverrideProperties["weights"]).Values.Select(item => item.Value));
        Assert.Equal([5f, 20f, 30f], ((ObservableFloatList)second.OverrideProperties["weights"]).Values.Select(item => item.Value));
    }

    [Fact]
    public async Task SpecializedAssetEdits_ChangeTypedSaveProperties_AndKeepMetadataSnapshotsIndependent()
    {
        var serializer = new WorkspaceManifestSerializer();
        var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
            new RecentWorkspaceStore(Path.Combine(Path.GetTempPath(), $"sailor-inspector-{Guid.NewGuid():N}.yaml")));
        await using var engine = new EngineService(lifecycle);
        MauiProgram.SetService(engine);
        using var assets = new AssetsService();
        try
        {
            var first = new TextureFile { FileId = new("first"), IsMetadataLoaded = true, ShouldGenerateMips = false };
            var second = new TextureFile { FileId = new("second"), IsMetadataLoaded = true, ShouldGenerateMips = true };
            first.AssetProperties.Add("bShouldGenerateMips", new Observable<bool>(false));
            second.AssetProperties.Add("bShouldGenerateMips", new Observable<bool>(true));
            var model = new InspectorAggregateBuilder(new WorldService(), assets, new EngineTypes()).Build([first, second]);
            var field = Assert.Single(model.Sections).Fields.Single(field => field.Name == "ShouldGenerateMips");
            Assert.Equal("*", field.DisplayValue);
            field.SetChoice("True");

            Assert.True(await model.CommitInspectorChangesAsync());

            Assert.True(first.ShouldGenerateMips);
            Assert.True(second.ShouldGenerateMips);
            Assert.False(((Observable<bool>)first.AssetProperties["bShouldGenerateMips"]).Value);
            Assert.True(first.IsDirty);
            Assert.True(second.IsDirty);
            Assert.True(model.IsDirty);
        }
        finally
        {
            MauiProgram.ClearServices();
        }
    }

    public static IEnumerable<object[]> SceneActions()
    {
        foreach (var name in new[] { "Add Component", "Paste Values", "Reset to Defaults", "Remove Component", "List Add", "List Clear", "List Remove [1]", "List Down [1]" })
        {
            yield return [name, false];
            yield return [name, true];
        }
    }

    [Theory]
    [MemberData(nameof(SceneActions))]
    public async Task SceneGroupActions_CompleteOneTransactionOrRequestRollbackWhenAnyTargetFails(string actionName, bool fail)
    {
        var world = new WorldService();
        var firstOwner = AddGameObject(world, "first-owner");
        var secondOwner = AddGameObject(world, "second-owner");
        var type = new ComponentType { Name = "Test::Weights", Properties = new() { ["weights"] = new Property<List<float>>() } };
        var first = AddComponent(world, firstOwner, "first", type, ("weights", new ObservableFloatList([1f, 2f])));
        var second = AddComponent(world, secondOwner, "second", type, ("weights", new ObservableFloatList([3f, 4f])));
        var transaction = new RecordingTransactions();
        var attempts = 0;
        Task<bool> Apply()
        {
            ++attempts;
            return Task.FromResult(!fail || attempts != 2);
        }
        first.Commit = _ => Apply();
        second.Commit = _ => Apply();
        world.AddComponentOperation = (_, _, _) => Apply();
        world.ResetComponentOperation = (_, _) => Apply();
        world.RemoveComponentOperation = (_, _) => Apply();
        MauiProgram.SetService(new ComponentClipboardService
        {
            PasteOperation = async (_, _) =>
            {
                if (!await Apply())
                {
                    throw new InvalidOperationException("Fixture failure");
                }
            }
        });
        SailorEditor.Views.AddComponentDialogPage.Choose = _ => Task.FromResult<string?>(type.Name);
        try
        {
            var model = new InspectorAggregateBuilder(world, null!, new EngineTypes(), transaction, new ContextProvider())
                .Build([firstOwner, secondOwner]);
            var component = model.Sections.Single(section => section.Name == "Weights");
            var action = actionName.StartsWith("List ", StringComparison.Ordinal)
                ? Assert.Single(component.Fields).Actions.Single(action => action.Name == actionName[5..])
                : actionName == "Add Component"
                    ? model.Sections.Single(section => section.Name == "Components").Actions!.Single()
                    : component.Actions!.Single(action => action.Name == actionName);

            if (fail)
            {
                await Assert.ThrowsAnyAsync<Exception>(() => action.Execute(CancellationToken.None));
                Assert.Equal(0, transaction.Completions);
                Assert.Equal(1, transaction.Rollbacks);
            }
            else
            {
                await action.Execute(CancellationToken.None);
                Assert.Equal(1, transaction.Completions);
                Assert.Equal(0, transaction.Rollbacks);
            }
            Assert.Equal(1, transaction.Begins);
            Assert.Equal(2, attempts);
        }
        finally
        {
            SailorEditor.Views.AddComponentDialogPage.Choose = _ => Task.FromResult<string?>(null);
            MauiProgram.ClearServices();
        }
    }

    [Fact]
    public async Task DeletedSceneActionTarget_FailsAndRequestsRollback()
    {
        var world = new WorldService();
        var owner = AddGameObject(world, "owner");
        var type = new ComponentType { Name = "Test::Component" };
        var first = AddComponent(world, owner, "first", type);
        var second = AddComponent(world, owner, "second", type);
        var transactions = new RecordingTransactions();
        var model = new InspectorAggregateBuilder(world, null!, new EngineTypes(), transactions, new ContextProvider()).Build([first, second]);
        world.Components.Remove("second");

        await Assert.ThrowsAsync<InvalidOperationException>(() => model.Sections[0].Actions!
            .Single(action => action.Name == "Reset to Defaults").Execute(CancellationToken.None));

        Assert.Equal(1, transactions.Begins);
        Assert.Equal(0, transactions.Completions);
        Assert.Equal(1, transactions.Rollbacks);
    }

    [Fact]
    public void PointerLeafEditors_PreserveReflectedAssetAndInstanceConstraints()
    {
        var world = new WorldService();
        var owner = AddGameObject(world, "owner");
        var descriptor = new ObjectPtrProperty { GenericType = typeof(TextureFile), GenericTypename = "Test::Texture" };
        var type = new ComponentType { Name = "Test::Pointer", Properties = new() { ["target"] = descriptor } };
        var first = AddComponent(world, owner, "first", type, ("target", new ObjectPtr()));
        var second = AddComponent(world, owner, "second", type, ("target", new ObjectPtr()));
        var model = new InspectorAggregateBuilder(world, null!, new EngineTypes()).Build([first, second]);
        var target = Assert.Single(Assert.Single(model.Sections).Fields);

        Assert.Equal(typeof(TextureFile), target.Children.Single(field => field.Name == "FileId").SupportedAssetType);
        Assert.Equal("Test::Texture", target.Children.Single(field => field.Name == "InstanceId").ExpectedInstanceType);
    }

    [Fact]
    public async Task OverlappingFieldsFromDifferentAggregateModels_SerializeTransactionsAndKeepSeparateUndoEntries()
    {
        var world = new WorldService();
        var owner = AddGameObject(world, "owner");
        var type = new ComponentType
        {
            Name = "Test::Pair",
            Properties = new() { ["left"] = new FloatProperty(), ["right"] = new FloatProperty() }
        };
        var first = AddComponent(world, owner, "first", type, ("left", new Observable<float>(1f)), ("right", new Observable<float>(10f)));
        var second = AddComponent(world, owner, "second", type, ("left", new Observable<float>(2f)), ("right", new Observable<float>(20f)));
        var published = new Dictionary<string, PublishedPair> { ["first"] = new(1f, 10f), ["second"] = new(2f, 20f) };
        var contextProvider = new ContextProvider();
        var dispatcher = new EditorCommandDispatcher(contextProvider, new InMemoryUndoRedoHistory());
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var firstCommand = true;
        foreach (var component in new[] { first, second })
        {
            component.Commit = async token =>
            {
                var next = new PublishedPair(((Observable<float>)component.OverrideProperties["left"]).Value,
                    ((Observable<float>)component.OverrideProperties["right"]).Value);
                var result = await dispatcher.DispatchAsync(new PublishPairCommand(published, component.InstanceId.Value, next, async cancellationToken =>
                {
                    if (firstCommand)
                    {
                        firstCommand = false;
                        started.SetResult();
                        await release.Task.WaitAsync(cancellationToken);
                    }
                }), contextProvider.GetCurrentContext(), token);
                return result.Succeeded;
            };
        }
        var firstModel = new InspectorAggregateBuilder(world, null!, new EngineTypes(), dispatcher, contextProvider).Build([first, second]);
        var left = firstModel.Sections[0].Fields.Single(property => property.Name == "left");
        left.SetDraft(9f);
        var leftCommit = left.CommitInspectorChangesAsync();
        await started.Task;

        var refreshedModel = new InspectorAggregateBuilder(world, null!, new EngineTypes(), dispatcher, contextProvider).Build([first, second]);
        var right = refreshedModel.Sections[0].Fields.Single(property => property.Name == "right");
        right.SetDraft(99f);
        var rightCommit = right.CommitInspectorChangesAsync();
        Assert.False(rightCommit.IsCompleted);
        release.SetResult();

        Assert.All(await Task.WhenAll(leftCommit, rightCommit), succeeded => Assert.True(succeeded));
        Assert.Equal(2, dispatcher.UndoCount);
        Assert.Equal(new PublishedPair(9f, 99f), published["first"]);
        Assert.Equal(new PublishedPair(9f, 99f), published["second"]);
        Assert.True(await dispatcher.UndoAsync());
        Assert.Equal(new PublishedPair(9f, 10f), published["first"]);
        Assert.Equal(new PublishedPair(9f, 20f), published["second"]);
        Assert.True(await dispatcher.UndoAsync());
        Assert.Equal(new PublishedPair(1f, 10f), published["first"]);
        Assert.Equal(new PublishedPair(2f, 20f), published["second"]);
    }

    sealed record PublishedPair(float Left, float Right);

    sealed class PublishPairCommand(Dictionary<string, PublishedPair> published, string id, PublishedPair next,
        Func<CancellationToken, Task> beforeExecute) : IUndoableEditorCommand
    {
        readonly PublishedPair previous = published[id];
        public string Name => nameof(PublishPairCommand);
        public string Description => "Publish test component";
        public bool CanExecute(ActionContext context) => true;
        public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            await beforeExecute(cancellationToken);
            published[id] = next;
            return CommandResult.Success();
        }
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            published[id] = previous;
            return ValueTask.FromResult(CommandResult.Success());
        }
    }

    sealed class ContextProvider : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null) => new(Origin: origin);
    }

    sealed class RecordingTransactions : ITransactionScopeFactory
    {
        public int Begins { get; private set; }
        public int Completions { get; private set; }
        public int Rollbacks { get; private set; }
        public ValueTask<ITransactionScope> BeginAsync(string description, ActionContext context, CancellationToken cancellationToken = default)
        {
            ++Begins;
            return ValueTask.FromResult<ITransactionScope>(new Scope(this, description));
        }
        sealed class Scope(RecordingTransactions owner, string description) : ITransactionScope
        {
            bool completed;
            public string Description => description;
            public ValueTask CompleteAsync(CancellationToken cancellationToken = default)
            {
                completed = true;
                ++owner.Completions;
                return ValueTask.CompletedTask;
            }
            public ValueTask DisposeAsync()
            {
                if (!completed)
                {
                    ++owner.Rollbacks;
                }
                return ValueTask.CompletedTask;
            }
        }
    }

    static GameObject AddGameObject(WorldService world, string id)
    {
        var gameObject = new GameObject { Name = id, InstanceId = new(id) };
        world.GameObjects.Add(id, gameObject);
        return gameObject;
    }

    static Component AddComponent(WorldService world, GameObject owner, string id, ComponentType type,
        params (string Name, ObservableObject Value)[] properties)
    {
        var component = new Component { InstanceId = new(id), Owner = owner, Typename = type };
        foreach (var property in properties)
        {
            component.OverrideProperties.Add(property.Name, property.Value);
        }
        world.Components.Add(id, component);
        return component;
    }

    static ObservableRecord Record(float strength, string label)
    {
        var record = new ObservableRecord();
        record.Fields.Add("strength", new Observable<float>(strength));
        record.Fields.Add("label", new Observable<string>(label));
        return record;
    }
}
