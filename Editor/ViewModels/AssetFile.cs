using Microsoft.Maui.Controls.Compatibility;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Services;
using System.Diagnostics;
using CommunityToolkit.Maui;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    public partial class AssetFile : ObservableObject
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

        public AssetUID UID { get { return Properties["fileId"].ToString(); } }
        public FileInfo Asset { get; set; }
        public FileInfo AssetInfo { get; set; }
        public Dictionary<string, object> Properties { get; set; } = new();
        public int Id { get; set; }
        public int FolderId { get; set; }
        public bool CanOpenAssetFile { get => !IsDirty; }
        public virtual async Task<bool> PreloadResources(bool force) => await Task.FromResult(true);
        public virtual async Task UpdateAssetFile()
        {
            await UpdateModel();

            using (var yamlAssetInfo = new FileStream(AssetInfo.FullName, FileMode.Create))
            using (var writer = new StreamWriter(yamlAssetInfo))
            {
                var serializer = new SerializerBuilder()
                    .WithNamingConvention(CamelCaseNamingConvention.Instance)
                    .Build();

                var yaml = serializer.Serialize(Properties);
                writer.Write(yaml);
            }

            IsDirty = false;
        }
        public async Task Revert()
        {
            await PreloadResources(true);

            IsDirty = false;
        }

        public void OpenAssetFile()
        {
            try
            {
                Process.Start(new ProcessStartInfo(Asset.FullName) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot open file: {ex.Message}");
            }
        }

        protected virtual async Task UpdateModel() { }
        public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }
        protected bool IsLoaded { get; set; }

        [ObservableProperty]
        protected bool isDirty = false;

        [ObservableProperty]
        protected string displayName;
    }
}