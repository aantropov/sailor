using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;
using System.Collections.Specialized;

namespace SailorEditor.ViewModels;

public partial class ObservableFileIdList : ObservableObject, ICloneable
{
    public ObservableFileIdList()
    {
        Values.CollectionChanged += ValuesCollectionChanged;
        Values.ItemChanged += ValuesItemChanged;
    }

    public ObservableFileIdList(IEnumerable<FileId> values) : this()
    {
        foreach (var value in values)
        {
            Values.Add(new Observable<FileId>(
                value ?? new FileId()));
        }
    }

    public ObservableList<Observable<FileId>> Values { get; } = [];

    public object Clone() => new ObservableFileIdList(Values.Select(value => (FileId)value.Value.Clone()));

    void ValuesCollectionChanged(object sender, NotifyCollectionChangedEventArgs e)
    {
        OnPropertyChanged(nameof(Values));
    }

    void ValuesItemChanged(object sender, ItemChangedEventArgs<Observable<FileId>> e)
    {
        OnPropertyChanged(nameof(Values));
    }
}

public partial class ObservableFloatList : ObservableObject, ICloneable
{
    public ObservableFloatList()
    {
        Values.CollectionChanged += ValuesCollectionChanged;
        Values.ItemChanged += ValuesItemChanged;
    }

    public ObservableFloatList(IEnumerable<float> values) : this()
    {
        foreach (var value in values)
        {
            Values.Add(new Observable<float>(value));
        }
    }

    public ObservableList<Observable<float>> Values { get; } = [];

    public object Clone() => new ObservableFloatList(Values.Select(value => value.Value));

    void ValuesCollectionChanged(object sender, NotifyCollectionChangedEventArgs e)
    {
        OnPropertyChanged(nameof(Values));
    }

    void ValuesItemChanged(object sender, ItemChangedEventArgs<Observable<float>> e)
    {
        OnPropertyChanged(nameof(Values));
    }
}
