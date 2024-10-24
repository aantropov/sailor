﻿using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.Runtime.CompilerServices;
using SailorEditor.Services;
using System.Windows.Input;
using SailorEngine;

namespace SailorEditor.ViewModels;

public partial class GameObject : ObservableObject, ICloneable
{
    public GameObject()
    {
        AddNewComponent = new Command(OnAddNewComponent);
        ClearComponentsCommand = new Command(OnClearComponents);
    }

    public string DisplayName { get { return Name; } }

    public int PrefabIndex = -1;

    public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }

    public List<Component> Components { get { return MauiProgram.GetService<WorldService>().GetComponents(this); } }

    [YamlMember(Alias = "components")]
    public List<int> ComponentIndices { get; set; } = [];

    public object Clone() => new GameObject()
    {
        Name = Name + "(Clone)",
        Position = new Vec4(Position),
        Rotation = new Rotation(Rotation),
        Scale = new Vec4(Scale),
        ParentIndex = ParentIndex,
        InstanceId = InstanceId,
        ComponentIndices = new List<int>(ComponentIndices)
    };

    public ICommand AddNewComponent { get; }
    public ICommand ClearComponentsCommand { get; }

    void OnAddNewComponent() { }
    void OnClearComponents() { }

    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    Vec4 position = new();

    [ObservableProperty]
    Rotation rotation = new();

    [ObservableProperty]
    Vec4 scale = new();

    [ObservableProperty]
    uint parentIndex = uint.MaxValue;

    [ObservableProperty]
    InstanceId instanceId = InstanceId.NullInstanceId;

    [ObservableProperty]
    protected bool isDirty = false;

}
