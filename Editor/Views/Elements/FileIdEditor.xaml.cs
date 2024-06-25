using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.Maui.Controls;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Threading.Tasks;

namespace SailorEditor.Views;

public partial class FileIdEditor : ContentView
{
    public static readonly BindableProperty FileIdProperty =
        BindableProperty.Create(
            nameof(FileId),
            typeof(FileId),
            typeof(FileIdEditor),
            default(FileId),
            BindingMode.TwoWay,
            propertyChanged: OnFileIdChanged);

    public FileId FileId
    {
        get => (FileId)GetValue(FileIdProperty);
        set => SetValue(FileIdProperty, value);
    }

    private static void OnFileIdChanged(BindableObject bindable, object oldValue, object newValue)
    {
        var control = (FileIdEditor)bindable;
        control.OnPropertyChanged(nameof(FileId));
        System.Diagnostics.Debug.WriteLine($"FileIdEditor: FileId changed from {oldValue} to {newValue}");
    }

    public FileIdEditorViewModel FileIdEditorViewModel { get; }

    public FileIdEditor()
    {
        InitializeComponent();
        FileIdEditorViewModel = new FileIdEditorViewModel(this);
        BindingContext = FileIdEditorViewModel;
    }
}

public class FileIdEditorViewModel : ObservableObject
{
    private readonly FileIdEditor _fileIdEditor;

    public FileIdEditorViewModel(FileIdEditor fileIdEditor)
    {
        _fileIdEditor = fileIdEditor;
        SelectFileCommand = new Command(async () => await OnSelectFile());
        ClearFileCommand = new Command(OnClearFile);
    }

    public FileId FileId
    {
        get => _fileIdEditor.FileId;
        set
        {
            if (_fileIdEditor.FileId != value)
            {
                _fileIdEditor.FileId = value;
                OnPropertyChanged();
                System.Diagnostics.Debug.WriteLine($"FileIdEditorViewModel: FileId set to {value}");
            }
        }
    }

    public Command SelectFileCommand { get; }
    public Command ClearFileCommand { get; }

    private async Task OnSelectFile()
    {
        var fileOpen = await FilePicker.Default.PickAsync();
        if (fileOpen != null)
        {
            var assetService = MauiProgram.GetService<AssetsService>();
            var asset = assetService.Files.Find(el => el.Asset.FullName == fileOpen.FullPath);
            FileId = asset?.FileId ?? default;
            System.Diagnostics.Debug.WriteLine($"FileIdEditorViewModel: File selected {FileId}");
        }
    }

    private void OnClearFile()
    {
        FileId = default;
        System.Diagnostics.Debug.WriteLine("FileIdEditorViewModel: File cleared");
    }
}