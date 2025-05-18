using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;
using System.Globalization;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.Collections.Generic;
using YamlDotNet.RepresentationModel;
using SailorEditor.Helpers;

namespace SailorEditor.ViewModels;

public partial class World : AssetFile
{
    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    ObservableList<Prefab> prefabs = [];

    public override async Task Save() => await Save(new WorldYamlConverter());

    public override async Task Revert()
    {
        try
        {
            var yaml = File.ReadAllText(AssetInfo.FullName);
            var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new WorldYamlConverter())
            .Build();

            var intermediateObject = deserializer.Deserialize<World>(yaml);

            FileId = intermediateObject.FileId;
            Filename = intermediateObject.Filename;

            DisplayName = Asset.Name;

            Prefabs.CollectionChanged += (a, e) => MarkDirty(nameof(Prefabs));
            Prefabs.ItemChanged += (a, e) => MarkDirty(nameof(Prefabs));

            IsLoaded = false;
        }
        catch (Exception ex)
        {
            DisplayName = ex.Message;
        }

        IsDirty = false;
    }
}

public class WorldYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(World);

    public object ReadYaml(IParser parser, Type type)
    {
        var commonConverters = new List<IYamlTypeConverter> {
                new RotationYamlConverter(),
                new Vec4YamlConverter(),
                new Vec3YamlConverter(),
                new Vec2YamlConverter(),
                new InstanceIdYamlConverter(),
                new FileIdYamlConverter(),
                new ComponentTypeYamlConverter(),
                new ViewModels.ComponentYamlConverter()
            };

        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ObservableListConverter<Prefab>(
                new IYamlTypeConverter[]{
                        new ObservableListConverter<GameObject>(commonConverters.ToArray()),
                        new ObservableListConverter<Component>(commonConverters.ToArray())
                }))
            .Build();

        var world = deserializer.Deserialize<World>(parser);

        foreach (var prefab in world.Prefabs)
        {
            foreach (var gameObject in prefab.GameObjects)
            {
                gameObject.Initialize();
                foreach (var component in prefab.Components)
                {
                    component.Initialize();
                }
            }
        }

        return world;
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        var world = (World)value;

        var commonConverters = new List<IYamlTypeConverter> {
                new RotationYamlConverter(),
                new Vec4YamlConverter(),
                new Vec3YamlConverter(),
                new Vec2YamlConverter(),
                new InstanceIdYamlConverter(),
                new FileIdYamlConverter(),
                new ComponentTypeYamlConverter(),
                new ViewModels.ComponentYamlConverter()
            };

        var serializerBuilder = SerializationUtils.CreateSerializerBuilder()
            .WithTypeConverter(new ObservableListConverter<Prefab>(
                new IYamlTypeConverter[]{
                        new ObservableListConverter<GameObject>(commonConverters.ToArray()),
                        new ObservableListConverter<Component>(commonConverters.ToArray())
                }));

        commonConverters.ForEach((el) => serializerBuilder.WithTypeConverter(el));

        var serializer = serializerBuilder.Build();

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        emitter.Emit(new Scalar(null, "name"));
        emitter.Emit(new Scalar(null, world.Name));

        emitter.Emit(new Scalar(null, "prefabs"));
        emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
        foreach (var item in world.Prefabs)
        {
            string itemYaml = serializer.Serialize(item);

            var yaml = new YamlStream();
            using (var sr = new StringReader(itemYaml))
            {
                yaml.Load(sr);
            }

            var doc = yaml.Documents[0].RootNode;
            YamlHelper.EmitNode(emitter, doc);
        }
        emitter.Emit(new SequenceEnd());

        emitter.Emit(new MappingEnd());
    }
}