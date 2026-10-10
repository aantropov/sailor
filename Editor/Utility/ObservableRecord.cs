using CommunityToolkit.Mvvm.ComponentModel;

namespace SailorEditor.Utility;

public sealed class ObservableRecord : ObservableObject, ICloneable
{
    public ObservableDictionary<string, ObservableObject> Fields { get; } = [];

    public ObservableRecord()
    {
        Fields.CollectionChanged += (_, _) => OnPropertyChanged(nameof(Fields));
        Fields.ValueChanged += (_, _) => OnPropertyChanged(nameof(Fields));
    }

    public object Clone()
    {
        var result = new ObservableRecord();
        foreach (var field in Fields)
            result.Fields.Add(field.Key, (ObservableObject)((ICloneable)field.Value).Clone());
        return result;
    }
}

public sealed class ObservableValueList : ObservableObject, ICloneable
{
    public ObservableList<ObservableObject> Values { get; } = [];

    public ObservableValueList()
    {
        Values.CollectionChanged += (_, _) => OnPropertyChanged(nameof(Values));
        Values.ItemChanged += (_, _) => OnPropertyChanged(nameof(Values));
    }

    public object Clone()
    {
        var result = new ObservableValueList();
        foreach (var value in Values)
            result.Values.Add((ObservableObject)((ICloneable)value).Clone());
        return result;
    }
}
