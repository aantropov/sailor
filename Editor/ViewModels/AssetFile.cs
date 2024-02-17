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

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    public class AssetFile : INotifyPropertyChanged
    {
        public AssetUID UID { get { return Properties["fileId"].ToString(); } }
        public FileInfo Asset { get; set; }
        public FileInfo AssetInfo { get; set; }
        public Dictionary<string, object> Properties { get; set; } = new();
        public string DisplayName
        {
            get { return displayName; }
            set { if (displayName != value) { displayName = value; MakeDirty(nameof(DisplayName)); } }
        }

        public int Id { get; set; }
        public int FolderId { get; set; }
        public bool IsDirty
        {
            get { return isDirty; }
            set { if (isDirty != value) { isDirty = value; OnPropertyChanged(nameof(IsDirty)); } }
        }
        public void MakeDirty([CallerMemberName] string propertyName = null)
        {
            IsDirty = true;
            OnPropertyChanged(propertyName);
        }
        public bool CanOpenAssetFile { get => !IsDirty; }

        public event PropertyChangedEventHandler PropertyChanged;
        protected virtual void OnPropertyChanged([CallerMemberName] string propertyName = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        public virtual bool PreloadResources(bool force) => true;
        public void UpdateAssetFile()
        {
            UpdateModel();

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
        public void Revert()
        {
            PreloadResources(true);
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

        protected bool IsLoaded { get; set; }
        protected virtual void UpdateModel() { }

        bool isDirty = false;
        string displayName;
    }
}