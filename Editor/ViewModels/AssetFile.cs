using System.Runtime.CompilerServices;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using SailorEngine;
using SailorEditor.Utility;
using System.Reflection;
using Microsoft.Maui.Storage;

namespace SailorEditor.ViewModels
{
    public partial class AssetFile : ObservableObject, ICloneable
    {
        public AssetFile()
        {
            PropertyChanged += (s, args) =>
            {
                if (args.PropertyName != "IsDirty")
                {
                    IsDirty = true;
                }
            };
        }

        public FileInfo Asset { get; set; }
        public FileInfo AssetInfo { get; set; }
        public int Id { get; set; }
        public int FolderId { get; set; }
        public bool CanOpenAssetFile { get => !IsDirty; }

        public virtual async Task<bool> LoadDependentResources() => await Task.FromResult(true);

        public virtual async Task Save() => await Save(new AssetFileYamlConverter());

        public virtual async Task Revert()
        {
            try
            {
                var yaml = File.ReadAllText(AssetInfo.FullName);
                var deserializer = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new AssetFileYamlConverter())
                .IgnoreUnmatchedProperties()
                .Build();

                var intermediateObject = deserializer.Deserialize<AssetFile>(yaml);

                FileId = intermediateObject.FileId;
                Filename = intermediateObject.Filename;

                DisplayName = Filename;

                IsDirty = false;
                IsLoaded = false;
            }
            catch (Exception ex)
            {
                DisplayName = ex.Message;
            }
        }

        public void Open() => Process.Start(new ProcessStartInfo(Asset.FullName) { UseShellExecute = true });

        public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }

        public object Clone() => new AssetFile();

        protected bool IsLoaded { get; set; }

        [ObservableProperty]
        protected bool isDirty = false;

        [ObservableProperty]
        protected string displayName;

        [ObservableProperty]
        FileId fileId;

        [ObservableProperty]
        FileId filename;

        protected async Task Save(IYamlTypeConverter converter)
        {
            using (var yamlAssetInfo = new FileStream(AssetInfo.FullName, FileMode.Create))
            using (var writer = new StreamWriter(yamlAssetInfo))
            {
                var serializer = new SerializerBuilder()
                    .WithNamingConvention(CamelCaseNamingConvention.Instance)
                    .WithTypeConverter(converter)
                    .Build();

                var yaml = serializer.Serialize(this);
                writer.Write(yaml);
            }

            IsDirty = false;
        }
    }

    public class AssetFileYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(AssetFile);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new FileIdYamlConverter())
                .IgnoreUnmatchedProperties()
                .Build();

            var assetFile = new AssetFile();

            // Move to the first mapping entry
            parser.Consume<MappingStart>();

            while (parser.Current is not MappingEnd)
            {
                if (parser.Current is Scalar scalar)
                {
                    var propertyName = scalar.Value;
                    parser.MoveNext(); // Move to the value

                    switch (propertyName)
                    {
                        case "fileId":
                            assetFile.FileId = deserializer.Deserialize<FileId>(parser);
                            break;
                        case "filename":
                            assetFile.Filename = deserializer.Deserialize<string>(parser);
                            break;
                        default:
                            // Skip other properties
                            deserializer.Deserialize<object>(parser);
                            break;
                    }
                }
                else
                {
                    parser.MoveNext();
                }
            }

            // Consume the end mapping
            parser.Consume<MappingEnd>();

            return assetFile;
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var assetFile = (AssetFile)value;
            var serializer = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new FileIdYamlConverter())
                .Build();

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            emitter.Emit(new Scalar(null, "fileId"));
            emitter.Emit(new Scalar(null, assetFile.FileId));

            emitter.Emit(new Scalar(null, "filename"));
            emitter.Emit(new Scalar(null, assetFile.Filename));

            emitter.Emit(new MappingEnd());
        }
    }
}