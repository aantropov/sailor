using SailorEditor.Content;
using YamlDotNet.RepresentationModel;

namespace Editor.Tests;

public sealed class PrefabOverrideApplierTests
{
    const string SourceYaml = """
        gameObjects:
          - name: Original Root
            position: [0, 1, 2, 1]
            rotation: [0, 0, 0, 1]
            scale: [1, 1, 1, 1]
            parentIndex: 4294967295
            instanceId: source-game-object
            components: [0]
        components:
          - typename: Sailor::TestComponent
            overrideProperties:
              value: 10
              untouched: original
              fileId: source-file
              instanceId: source-component
        """;

    const string LinkedYaml = """
        fileId: source-prefab
        instanceIds:
          source-game-object: live-game-object
          source-component: live-component
        gameObjectOverrides:
          source-game-object:
            name: Applied Root
            position: [7, 8, 9, 1]
        componentOverrides:
          source-component:
            typename: Sailor::TestComponent
            overrideProperties:
              value: 42
              added: applied
              fileId: live-file
              instanceId: live-component
        """;

    [Fact]
    public void Apply_MergesOverridesAndPreservesStableSourceIdentity()
    {
        var result = PrefabOverrideApplier.Apply(SourceYaml, LinkedYaml);
        var root = ParseRoot(result);
        var gameObject = Sequence(root, "gameObjects").Children
            .OfType<YamlMappingNode>()
            .Single();
        var component = Sequence(root, "components").Children
            .OfType<YamlMappingNode>()
            .Single();
        var properties = Mapping(component, "overrideProperties");

        Assert.Equal("Applied Root", Scalar(gameObject, "name"));
        Assert.Equal("[0, 1, 2, 1]", InlineSequence(gameObject, "position"));
        Assert.Equal("source-game-object", Scalar(gameObject, "instanceId"));
        Assert.Equal("42", Scalar(properties, "value"));
        Assert.Equal("applied", Scalar(properties, "added"));
        Assert.Equal("original", Scalar(properties, "untouched"));
        Assert.Equal("source-file", Scalar(properties, "fileId"));
        Assert.Equal("source-component", Scalar(properties, "instanceId"));
    }

    [Fact]
    public void Apply_IgnoresOverridesForUnknownSourceObjects()
    {
        var linked = LinkedYaml.ReplaceLineEndings("\n").Replace(
            "source-game-object:\n    name: Applied Root",
            "unknown-game-object:\n    name: Applied Root",
            StringComparison.Ordinal);

        var result = PrefabOverrideApplier.Apply(SourceYaml, linked);
        var gameObject = Sequence(ParseRoot(result), "gameObjects").Children
            .OfType<YamlMappingNode>()
            .Single();

        Assert.Equal("Original Root", Scalar(gameObject, "name"));
        Assert.Equal("source-game-object", Scalar(gameObject, "instanceId"));
    }

    [Fact]
    public void Apply_CopiesCompleteExpandedComponentValuesIncludingReferences()
    {
        const string source = """
            gameObjects:
              - name: Saxophone
                position: [0, 0, 0, 1]
                rotation: [0, 0, 0, 1]
                scale: [1, 1, 1, 1]
                parentIndex: 4294967295
                instanceId: source-object
                components: [0]
            components:
              - typename: Sailor::AudioSourceComponent
                overrideProperties:
                  clip: ~
                  volume: 0.5
                  fileId: source-file
                  instanceId: audio_source-object
            """;
        const string linked = """
            fileId: source-prefab
            instanceIds:
              source-object: live-object
            gameObjects:
              - name: Edited Saxophone
                position: [4, 5, 6, 1]
                rotation: [0, 0, 0, 1]
                scale: [2, 2, 2, 1]
                parentIndex: 4294967295
                instanceId: live-object
                components: [0]
            components:
              - typename: Sailor::AudioSourceComponent
                overrideProperties:
                  clip:
                    fileId: AUDIO-FILE
                    instanceId: ''
                  volume: 0.8
                  fileId: live-file
                  instanceId: audio_live-object
            """;

        var result = PrefabOverrideApplier.Apply(source, linked);
        var root = ParseRoot(result);
        var gameObject = Sequence(root, "gameObjects").Children
            .OfType<YamlMappingNode>()
            .Single();
        var component = Sequence(root, "components").Children
            .OfType<YamlMappingNode>()
            .Single();
        var properties = Mapping(component, "overrideProperties");
        var clip = Mapping(properties, "clip");

        Assert.Equal("Edited Saxophone", Scalar(gameObject, "name"));
        Assert.Equal("[0, 0, 0, 1]", InlineSequence(gameObject, "position"));
        Assert.Equal("[2, 2, 2, 1]", InlineSequence(gameObject, "scale"));
        Assert.Equal("source-object", Scalar(gameObject, "instanceId"));
        Assert.Equal("0.8", Scalar(properties, "volume"));
        Assert.Equal("AUDIO-FILE", Scalar(clip, "fileId"));
        Assert.Equal("source-file", Scalar(properties, "fileId"));
        Assert.Equal("audio_source-object", Scalar(properties, "instanceId"));
    }

    static YamlMappingNode ParseRoot(string yaml)
    {
        var stream = new YamlStream();
        stream.Load(new StringReader(yaml));
        return Assert.IsType<YamlMappingNode>(
            Assert.Single(stream.Documents).RootNode);
    }

    static YamlMappingNode Mapping(YamlMappingNode parent, string key) =>
        Assert.IsType<YamlMappingNode>(parent.Children[new YamlScalarNode(key)]);

    static YamlSequenceNode Sequence(YamlMappingNode parent, string key) =>
        Assert.IsType<YamlSequenceNode>(parent.Children[new YamlScalarNode(key)]);

    static string Scalar(YamlMappingNode parent, string key) =>
        Assert.IsType<YamlScalarNode>(
            parent.Children[new YamlScalarNode(key)]).Value!;

    static string InlineSequence(YamlMappingNode parent, string key) =>
        $"[{string.Join(", ", Sequence(parent, key).Children
            .Cast<YamlScalarNode>()
            .Select(node => node.Value))}]";
}
