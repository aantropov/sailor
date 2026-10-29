using SailorEditor.Utility;
using SailorEngine;
using SailorEditor.Commands;
using SailorEditor.History;
using SailorEditor.ViewModels;

namespace Editor.Tests;

public sealed class InspectorAggregateEditingTests
{
    [Fact]
    public async Task MixedField_PreservesOriginalsUntilExplicitEdit_EvenWhenChoosingPrimaryValue()
    {
        var values = new object?[] { 2f, 7f };
        var field = Field(values, typeof(float));

        Assert.True(field.IsMixed);
        Assert.Equal("*", field.DisplayValue);
        Assert.False(field.HasPendingInspectorChanges);
        Assert.True(await field.CommitInspectorChangesAsync());
        Assert.Equal(new object?[] { 2f, 7f }, values);

        field.SetInputText("2");
        Assert.True(field.HasPendingInspectorChanges);
        Assert.Equal(new object?[] { 2f, 7f }, values);
        Assert.True(await field.CommitInspectorChangesAsync());

        Assert.Equal(new object?[] { 2f, 2f }, values);
        Assert.False(field.IsMixed);
        Assert.Equal("2", field.DisplayValue);
    }

    [Theory]
    [InlineData("*")]
    [InlineData("-")]
    [InlineData("NaN")]
    [InlineData("Infinity")]
    public async Task InvalidNumericInput_DoesNotReplaceDifferentValues(string text)
    {
        var values = new object?[] { 2f, 7f };
        var field = Field(values, typeof(float));
        field.SetInputText(text);

        Assert.True(await field.CommitInspectorChangesAsync());
        Assert.Equal(new object?[] { 2f, 7f }, values);
        Assert.Equal("*", field.DisplayValue);
        Assert.False(field.HasPendingInspectorChanges);
    }

    [Fact]
    public async Task MixedStrings_CanBeExplicitlyChangedToLiteralAsterisk()
    {
        var values = new object?[] { "first", "second" };
        var field = Field(values, typeof(string));
        field.SetInputText("*");

        Assert.True(await field.CommitInspectorChangesAsync());
        Assert.Equal(new object?[] { "*", "*" }, values);
        Assert.False(field.IsMixed);
    }

    [Fact]
    public async Task ReferenceEdits_CloneIdsForEveryTarget()
    {
        var values = new object?[] { new FileId("first"), new FileId("second") };
        var field = Field(values, typeof(FileId));
        var replacement = new FileId("replacement");
        field.SetDraft(replacement);
        replacement.Value = "modified-after-typing";

        Assert.True(await field.CommitInspectorChangesAsync());
        Assert.Equal("replacement", Assert.IsType<FileId>(values[0]).Value);
        Assert.Equal("replacement", Assert.IsType<FileId>(values[1]).Value);
        Assert.NotSame(values[0], values[1]);
        Assert.NotSame(replacement, values[0]);
    }

    [Fact]
    public async Task FailedTarget_RetainsPendingEditAndRetriesWithoutLosingValues()
    {
        var first = 1f;
        var second = 2f;
        var fail = true;
        var field = new InspectorAggregateField("Intensity", typeof(float),
        [
            new(() => first, (value, _) => { first = (float)value!; return Task.FromResult(true); }),
            new(() => second, (value, _) =>
            {
                if (fail)
                {
                    return Task.FromResult(false);
                }
                second = (float)value!;
                return Task.FromResult(true);
            })
        ]);
        field.SetDraft(9f);

        Assert.False(await field.CommitInspectorChangesAsync());
        Assert.True(field.HasPendingInspectorChanges);
        Assert.Equal(9f, first);
        Assert.Equal(2f, second);
        fail = false;

        Assert.True(await field.CommitInspectorChangesAsync());
        Assert.Equal(9f, second);
        Assert.False(field.HasPendingInspectorChanges);
    }

    [Fact]
    public async Task TextEditedDuringCommit_IsAppliedBeforeTheCommitCompletes()
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var value = 1f;
        var first = true;
        var field = new InspectorAggregateField("Value", typeof(float),
        [
            new(() => value, async (next, token) =>
            {
                if (first)
                {
                    first = false;
                    started.SetResult();
                    await release.Task.WaitAsync(token);
                }
                value = (float)next!;
                return true;
            })
        ]);
        field.SetInputText("2");
        var commit = field.CommitInspectorChangesAsync();
        await started.Task;
        field.SetInputText("3");
        release.SetResult();

        Assert.True(await commit);
        Assert.Equal(3f, value);
        Assert.False(field.HasPendingInspectorChanges);
    }

    [Fact]
    public async Task SaveAndRevert_UseTheWholeSelection_AndDiscardUncommittedTextOnRevert()
    {
        var values = new object?[] { 1, 2 };
        var field = Field(values, typeof(int));
        var saves = 0;
        var reverts = 0;
        var model = new AggregateInspectorModel(2, [new("Common Properties", [field])], true,
            save: _ => { ++saves; return Task.FromResult(true); },
            revert: _ => { ++reverts; values[0] = 1; values[1] = 2; return Task.CompletedTask; });
        field.SetInputText("5");

        Assert.True(await model.SaveAsync());
        Assert.Equal(new object?[] { 5, 5 }, values);
        Assert.Equal(1, saves);
        field.SetInputText("8");
        await model.RevertAsync();

        Assert.Equal(new object?[] { 1, 2 }, values);
        Assert.Equal(1, reverts);
        Assert.False(model.HasPendingInspectorChanges);
        Assert.Equal("*", field.DisplayValue);
    }

    [Fact]
    public void RecordAndListEquality_UsesNestedValues()
    {
        var first = new ObservableRecord();
        first.Fields.Add("weights", new SailorEditor.ViewModels.ObservableFloatList([1f, 2f]));
        var second = (ObservableRecord)first.Clone();

        Assert.True(InspectorAggregateField.ValuesEqual(first, second));
        ((SailorEditor.ViewModels.ObservableFloatList)second.Fields["weights"]).Values[1].Value = 9f;
        Assert.False(InspectorAggregateField.ValuesEqual(first, second));
        Assert.Equal(2f, ((SailorEditor.ViewModels.ObservableFloatList)first.Fields["weights"]).Values[1].Value);
    }

    [Fact]
    public void ReadOnlyField_IgnoresEveryEditPath()
    {
        var values = new object?[] { 1, 2 };
        var targets = values.Select((_, index) => new InspectorAggregateValueTarget(
            () => values[index], (value, _) => { values[index] = value; return Task.FromResult(true); })).ToArray();
        var field = new InspectorAggregateField("Read Only", typeof(int), targets) { IsReadOnly = true };

        field.SetDraft(4);
        field.SetInputText("5");
        Assert.False(field.SetText("6"));
        Assert.False(field.HasPendingInspectorChanges);
        Assert.Equal(new object?[] { 1, 2 }, values);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SceneFieldTransaction_GroupsUndoAndRollsBackFailedTargets(bool failSecondTarget)
    {
        var values = new object?[] { 1, 2 };
        var context = new ActionContext();
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new TestContextProvider(), history);
        var targets = values.Select((_, index) => new InspectorAggregateValueTarget(
            () => values[index], async (value, token) =>
            {
                var result = await dispatcher.DispatchAsync(new ChangeValueCommand(values, index, value,
                    failSecondTarget && index == 1), context, token);
                return result.Succeeded;
            })).ToArray();
        var property = new InspectorAggregateField("Value", typeof(int), targets)
        {
            ApplyGroup = async (apply, token) =>
            {
                await using var transaction = await dispatcher.BeginAsync("Edit selection", context, token);
                if (!await apply())
                {
                    return false;
                }
                await transaction.CompleteAsync(token);
                return true;
            }
        };
        property.SetDraft(9);

        Assert.Equal(!failSecondTarget, await property.CommitInspectorChangesAsync());
        if (failSecondTarget)
        {
            Assert.Equal(new object?[] { 1, 2 }, values);
            Assert.Equal(0, dispatcher.UndoCount);
            Assert.True(property.HasPendingInspectorChanges);
        }
        else
        {
            Assert.Equal(new object?[] { 9, 9 }, values);
            Assert.Equal(1, dispatcher.UndoCount);
            Assert.True(await dispatcher.UndoAsync());
            Assert.Equal(new object?[] { 1, 2 }, values);
        }
    }

    [Fact]
    public void MaterialUniformClones_RetainConcreteTypesAndOwnTheirNestedValues()
    {
        var floats = new ObservableList<UniformFloat> { new() { Key = "roughness", Value = 0.5f } };
        var samplers = new ObservableList<UniformFileId> { new() { Key = "albedo", Value = new FileId("original") } };
        var vectors = new ObservableList<UniformVec4> { new() { Key = "tint", Value = new SailorEditor.Vec4 { X = 1f, Y = 2f } } };

        var floatCopy = Assert.IsType<ObservableList<UniformFloat>>(InspectorAggregateField.CloneValue(floats));
        var samplerCopy = Assert.IsType<ObservableList<UniformFileId>>(InspectorAggregateField.CloneValue(samplers));
        var vectorCopy = Assert.IsType<ObservableList<UniformVec4>>(InspectorAggregateField.CloneValue(vectors));

        Assert.IsType<UniformFloat>(Assert.Single(floatCopy));
        Assert.IsType<UniformFileId>(Assert.Single(samplerCopy));
        Assert.IsType<UniformVec4>(Assert.Single(vectorCopy));
        floatCopy[0].Value = 0.75f;
        samplerCopy[0].Value.Value = "changed";
        vectorCopy[0].Value.Y = 9f;
        Assert.Equal(0.5f, floats[0].Value);
        Assert.Equal("original", samplers[0].Value.Value);
        Assert.Equal(2f, vectors[0].Value.Y);
    }

    sealed class TestContextProvider : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null) => new(Origin: origin);
    }

    sealed class ChangeValueCommand(object?[] values, int index, object? value, bool fail) : IUndoableEditorCommand
    {
        readonly object? previous = values[index];
        public string Name => nameof(ChangeValueCommand);
        public string Description => "Change value";
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            if (fail)
            {
                return Task.FromResult(CommandResult.Failure());
            }
            values[index] = value;
            return Task.FromResult(CommandResult.Success());
        }
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            values[index] = previous;
            return ValueTask.FromResult(CommandResult.Success());
        }
    }

    static InspectorAggregateField Field(object?[] values, Type type) => new("Value", type,
        values.Select((_, index) => new InspectorAggregateValueTarget(
            () => values[index],
            (value, _) => { values[index] = value; return Task.FromResult(true); })).ToArray());
}
