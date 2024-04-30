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
using SailorEditor.Utility;
using YamlDotNet.Core.Tokens;

namespace SailorEditor.ViewModels
{
    public class AssetUID : IComparable<AssetUID>, IComparable<string>, ICloneable
    {
        public AssetUID() { }
        public AssetUID(string v) { Value = v; }

        public string Value = "";

        public object Clone() => new AssetUID() { Value = Value };
        public int CompareTo(AssetUID other) => Value.CompareTo(other.Value);
        public int CompareTo(string other) => Value.CompareTo(other);

        public override bool Equals(object obj)
        {
            if (obj is AssetUID other)
            {
                return Value.CompareTo(other.Value) == 0;
            }
            else if (obj is string str)
            {
                return Value.CompareTo(str) == 0;
            }

            return false;
        }

        public override int GetHashCode() => Value?.GetHashCode() ?? 0;

        public static implicit operator string(AssetUID ts)
        {
            return ((ts == null) ? null : ts.Value);
        }
        public static implicit operator AssetUID(string val)
        {
            return new AssetUID { Value = val };
        }
    }

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

        public object Clone() => new AssetFile();

        protected bool IsLoaded { get; set; }

        [ObservableProperty]
        protected bool isDirty = false;

        [ObservableProperty]
        protected string displayName;
    }
}