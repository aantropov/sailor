using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class EnumPickerBehavior : Behavior<Picker>
{
    public static readonly BindableProperty ValuesProperty = BindableProperty.Create(
        nameof(Values),
        typeof(IList<string>),
        typeof(EnumPickerBehavior),
        Array.Empty<string>(),
        propertyChanged: OnEnumBindingChanged);

    public static readonly BindableProperty ValueProperty = BindableProperty.Create(
        nameof(Value),
        typeof(string),
        typeof(EnumPickerBehavior),
        string.Empty,
        BindingMode.TwoWay,
        propertyChanged: OnEnumBindingChanged);

    Picker picker;
    bool updatingSelection;

    public IList<string> Values
    {
        get => (IList<string>)GetValue(ValuesProperty);
        set => SetValue(ValuesProperty, value);
    }

    public string Value
    {
        get => (string)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    protected override void OnAttachedTo(Picker bindable)
    {
        base.OnAttachedTo(bindable);

        picker = bindable;
        BindingContext = bindable.BindingContext;
        picker.BindingContextChanged += OnPickerBindingContextChanged;
        picker.Loaded += OnPickerLoaded;
        picker.Focused += OnPickerFocused;
        picker.SelectedIndexChanged += OnPickerSelectedIndexChanged;
        RefreshSelection();
    }

    protected override void OnDetachingFrom(Picker bindable)
    {
        bindable.BindingContextChanged -= OnPickerBindingContextChanged;
        bindable.Loaded -= OnPickerLoaded;
        bindable.Focused -= OnPickerFocused;
        bindable.SelectedIndexChanged -= OnPickerSelectedIndexChanged;
        picker = null;

        base.OnDetachingFrom(bindable);
    }

    static void OnEnumBindingChanged(BindableObject bindable, object oldValue, object newValue)
    {
        ((EnumPickerBehavior)bindable).RefreshSelection();
    }

    void OnPickerBindingContextChanged(object sender, EventArgs e)
    {
        BindingContext = picker.BindingContext;
        RefreshSelection();
    }

    void OnPickerLoaded(object sender, EventArgs e) => RefreshSelection();

    void OnPickerFocused(object sender, FocusEventArgs e) => RefreshSelection();

    void OnPickerSelectedIndexChanged(object sender, EventArgs e)
    {
        if (updatingSelection ||
            picker == null ||
            picker.SelectedIndex < 0)
        {
            return;
        }

        var values = Values ?? Array.Empty<string>();
        if (picker.SelectedIndex >= values.Count)
        {
            return;
        }

        Value = values[picker.SelectedIndex];
    }

    void RefreshSelection()
    {
        if (picker == null)
        {
            return;
        }

        var values = Values ?? Array.Empty<string>();
        var value = EnumValueUtils.Normalize(Value);
        var index = -1;
        for (var i = 0; i < values.Count; i++)
        {
            if (string.Equals(EnumValueUtils.Normalize(values[i]), value, StringComparison.Ordinal))
            {
                index = i;
                break;
            }
        }

        updatingSelection = true;
        picker.SelectedIndex = index;
        picker.SelectedItem = index >= 0 && index < values.Count ? values[index] : null;
        updatingSelection = false;
    }
}
