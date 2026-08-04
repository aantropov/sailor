using SailorEditor.ViewModels;
using CheckBox = Microsoft.Maui.Controls.CheckBox;
using Grid = Microsoft.Maui.Controls.Grid;
using SkiaSharp;
using Entry = Microsoft.Maui.Controls.Entry;
using DataTemplate = Microsoft.Maui.Controls.DataTemplate;
using Button = Microsoft.Maui.Controls.Button;
using System.Collections.ObjectModel;
using BindingMode = Microsoft.Maui.Controls.BindingMode;
using IValueConverter = Microsoft.Maui.Controls.IValueConverter;
using SailorEditor.Utility;
using System.ComponentModel;
using SailorEditor.Services;
using CommunityToolkit.Maui.Markup;
using System.Linq.Expressions;
using SailorEngine;
using Component = SailorEditor.ViewModels.Component;
using SailorEditor.Views;
using SailorEditor.Controls;
using System;
namespace SailorEditor.Helpers;
static class Templates
{
    static void CommitInspectorBindingContext(object? bindingContext)
    {
        if (bindingContext is IInspectorEditable editable && editable.HasPendingInspectorChanges)
            _ = CommitInspectorBindingContextAsync(editable);
    }

    static async Task CommitInspectorBindingContextAsync(IInspectorEditable editable)
    {
        try
        {
            await editable.CommitInspectorChangesAsync();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Inspector commit failed: {ex}");
        }
    }

    static void ScheduleInspectorCommit(Entry entry)
    {
        if (entry.BindingContext is not IInspectorEditable editable)
            return;

        entry.Dispatcher.DispatchDelayed(TimeSpan.FromMilliseconds(1), async () =>
        {
            if (!editable.HasPendingInspectorChanges)
                return;

            try
            {
                await editable.CommitInspectorChangesAsync();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector commit failed: {ex}");
            }
        });
    }

    public const int ThumbnailSize = 128;
    public const double InspectorLabelColumnWidth = 140;
    public const double InspectorFieldSpacing = 6;

    public static Microsoft.Maui.Controls.Editor ReadOnlyTextView<T>(Expression<Func<T, string>> prop)
        where T : class
    {
        var editor = new Microsoft.Maui.Controls.Editor
        {
            FontSize = 12,
            IsReadOnly = true,
            AutoSize = Microsoft.Maui.Controls.EditorAutoSizeOption.TextChanges
        };

        editor.Bind(Microsoft.Maui.Controls.Editor.TextProperty, getter: prop);
        return editor;
    }

    public static View ShaderCodeView(string title, Expression<Func<ShaderFile, string>> prop)
    {
        var titleLabel = new Label
        {
            Text = title,
            VerticalOptions = LayoutOptions.Center
        };

        var codeView = ReadOnlyTextView(prop);

        var stackLayout = new VerticalStackLayout();
        stackLayout.Children.Add(titleLabel);
        stackLayout.Children.Add(codeView);

        return stackLayout;
    }

    public static CheckBox CheckBox<T>(Expression<Func<T, bool>> getter, Action<T, bool> setter)
        where T : class
    {
        var checkBox = new Microsoft.Maui.Controls.CheckBox();
        checkBox.Bind(Microsoft.Maui.Controls.CheckBox.IsCheckedProperty, getter: getter, setter: setter);
        return checkBox;
    }

    public static View BoolEditor<TBindingContext>(Expression<Func<TBindingContext, bool>> getter, Action<TBindingContext, bool> setter)
        where TBindingContext : class
    {
        var checkBox = CheckBox(getter, setter);
        checkBox.CheckedChanged += (sender, args) => CommitInspectorBindingContext(((CheckBox)sender).BindingContext);
        return checkBox;
    }

    public static Picker EnumPicker<TBinding, TEnum>(Expression<Func<TBinding, TEnum>> getter, Action<TBinding, TEnum> setter)
        where TBinding : class
        where TEnum : struct
    {
        var picker = new Picker
        {
            FontSize = 12
        };

        var enumValues = Enum.GetValues(typeof(TEnum)).Cast<TEnum>();
        foreach (var value in enumValues)
        {
            picker.Items.Add(value.ToString());
        }

        picker.Bind<Picker, TBinding, TEnum, string>(Picker.SelectedItemProperty, getter: getter, setter: setter, BindingMode.TwoWay, new EnumToStringConverter<TEnum>());
        MainThread.BeginInvokeOnMainThread(() =>
        {
            picker.SelectedIndexChanged += (sender, _) => CommitInspectorBindingContext(((Picker)sender).BindingContext);
        });
        return picker;
    }

    public static Picker EnumPicker<TBinding>(List<string> enumValues, Expression<Func<TBinding, string>> getter, Action<TBinding, string> setter)
    {
        var picker = new Picker
        {
            FontSize = 12
        };

        var values = enumValues ?? [];
        foreach (var value in values)
        {
            picker.Items.Add(value);
        }

        var compiledGetter = getter.Compile();
        bool bUpdating = false;

        void RefreshSelectedItem()
        {
            if (picker.BindingContext is not TBinding context)
            {
                return;
            }

            var value = EnumValueUtils.Normalize(compiledGetter(context));
            var index = values.FindIndex(x => string.Equals(x, value, StringComparison.Ordinal));
            bUpdating = true;
            picker.SelectedIndex = index;
            bUpdating = false;
        }

        picker.BindingContextChanged += (sender, _) => RefreshSelectedItem();
        picker.SelectedIndexChanged += (sender, _) =>
        {
            if (bUpdating || picker.BindingContext is not TBinding context)
            {
                return;
            }

            if (picker.SelectedIndex >= 0 && picker.SelectedIndex < values.Count)
            {
                setter(context, values[picker.SelectedIndex]);
                CommitInspectorBindingContext(context);
            }
        };

        MainThread.BeginInvokeOnMainThread(RefreshSelectedItem);
        return picker;
    }

    public static View EntryField<TBindingContext, TSource>(Expression<Func<TBindingContext, TSource>> getter, Action<TBindingContext, TSource> setter, IValueConverter valueConverter = null)
        where TBindingContext : class
    {
        var entry = CreateInspectorEntry();
        entry.Bind<Entry, TBindingContext, TSource, string>(Entry.TextProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay, converter: valueConverter);
        return entry;
    }

    public static View FileIdLabel<TBindingContext>(string bindingPath, Expression<Func<TBindingContext, FileId>> getter, Action<TBindingContext, FileId> setter)
        where TBindingContext : class
    {
        var label = new Label();
        label.Behaviors.Add(new FileIdSelectBehavior());
        label.Bind<Label, TBindingContext, FileId, string>(Label.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.Default,
            converter: new FileIdToFilenameConverter());

        return label;
    }

    static Entry CreateInspectorEntry()
    {
        return new Entry
        {
            FontSize = 12,
            HorizontalOptions = LayoutOptions.Fill,
            MinimumWidthRequest = 0
        };
    }

    static Grid CreateInlineFieldGrid(params View[] views)
    {
        var grid = new Grid
        {
            ColumnSpacing = InspectorFieldSpacing,
            HorizontalOptions = LayoutOptions.Fill,
            MinimumWidthRequest = 0
        };

        for (var i = 0; i < views.Length; i++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Star });
            var view = views[i];
            if (view is View element)
            {
                element.HorizontalOptions = LayoutOptions.Fill;
                element.MinimumWidthRequest = 0;
            }

            grid.Add(view, i, 0);
        }

        return grid;
    }

    static Label CreateInspectorValueLabel()
    {
        return new Label
        {
            HorizontalOptions = LayoutOptions.Fill,
            VerticalOptions = LayoutOptions.Center,
            LineBreakMode = LineBreakMode.WordWrap,
            MaxLines = 3,
            MinimumWidthRequest = 0
        };
    }

    public static View TextureEditor<TBindingContext>(Expression<Func<TBindingContext, FileId>> getter, Action<TBindingContext, FileId> setter)
    {
        var selectButton = new Button { Text = "Select" };
        selectButton.Clicked += async (sender, e) =>
        {
            var fileOpen = await FilePicker.Default.PickAsync();
            if (fileOpen != null)
            {
                var AssetService = MauiProgram.GetService<AssetsService>();
                var asset = AssetService.Files.Find((el) => el.Asset.FullName == fileOpen.FullPath);
                setter((TBindingContext)(sender as Button).BindingContext, asset.FileId is FileId id ? id : default);
            }
        };

        var image = new Image
        {
            WidthRequest = 64,
            HeightRequest = 64,
            Aspect = Aspect.AspectFit,
            HorizontalOptions = LayoutOptions.Start,
            VerticalOptions = LayoutOptions.Start
        };

        image.Bind<Image, Uniform<FileId>, FileId, Image>(Image.SourceProperty,
            mode: BindingMode.Default,
            converter: new FileIdToPreviewTextureConverter(),
            getter: static (Uniform<FileId> vm) => vm.Value);

        var valueEntry = CreateInspectorValueLabel();

        valueEntry.Bind<Label, Uniform<FileId>, FileId, string>(Label.TextProperty,
            mode: BindingMode.Default,
            converter: new FileIdToFilenameConverter(),
            getter: static (Uniform<FileId> vm) => vm.Value);

        valueEntry.Behaviors.Add(new FileIdSelectBehavior());

        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = GridLength.Auto },
                new ColumnDefinition { Width = GridLength.Auto },
                new ColumnDefinition { Width = GridLength.Star }
            },
            ColumnSpacing = InspectorFieldSpacing
        };

        grid.Add(selectButton, 0, 0);
        grid.Add(image, 1, 0);
        grid.Add(valueEntry, 2, 0);

        return grid;
    }

    public static View FileIdEditor<TBindingContext>(object bindingContext, string bindingPath, Expression<Func<TBindingContext, FileId>> getter, Action<TBindingContext, FileId> setter, Type supportedType = null)
        where TBindingContext : class
    {
        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, e) =>
        {
            setter((TBindingContext)bindingContext, new FileId());
            CommitInspectorBindingContext(bindingContext);
        };

        var valueEntry = CreateInspectorValueLabel();

        valueEntry.BindingContext = bindingContext;

        valueEntry.Bind<Label, TBindingContext, FileId, string>(Label.TextProperty,
            mode: BindingMode.TwoWay,
            converter: new FileIdToFilenameConverter(),
            getter: getter,
            setter: setter);

        var dragAndDropBehaviour = new FileIdDragAndDropBehaviour();
        dragAndDropBehaviour.SetBinding(FileIdDragAndDropBehaviour.BoundPropertyProperty, new Binding(bindingPath));
        dragAndDropBehaviour.SupportedType = supportedType;

        var selectBehavior = new FileIdSelectBehavior();
        selectBehavior.SetBinding(FileIdSelectBehavior.BoundPropertyProperty, new Binding(bindingPath));

        valueEntry.Behaviors.Add(dragAndDropBehaviour);
        valueEntry.Behaviors.Add(selectBehavior);

        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = GridLength.Auto },
                new ColumnDefinition { Width = GridLength.Star }
            },
            ColumnSpacing = InspectorFieldSpacing
        };

        grid.Add(clearButton, 0, 0);
        grid.Add(valueEntry, 1, 0);

        return grid;
    }

    public static View FileIdListEditor(
        ObservableFileIdList fileIds,
        Type supportedType = null)
    {
        var listEditor = new VerticalStackLayout
        {
            HorizontalOptions = LayoutOptions.Fill,
            Spacing = InspectorFieldSpacing
        };
        BindableLayout.SetItemsSource(listEditor, fileIds.Values);
        BindableLayout.SetItemTemplate(
            listEditor,
            new DataTemplate(() =>
            {
                var row = new Grid
                {
                    ColumnDefinitions =
                    {
                        new ColumnDefinition { Width = GridLength.Auto },
                        new ColumnDefinition { Width = GridLength.Star }
                    },
                    ColumnSpacing = InspectorFieldSpacing,
                    HorizontalOptions = LayoutOptions.Fill
                };

                row.BindingContextChanged += (sender, args) =>
                {
                    row.Children.Clear();
                    if (row.BindingContext is not Observable<FileId> fileId)
                    {
                        return;
                    }

                    var removeButton = new Button { Text = "-" };
                    removeButton.Clicked += (buttonSender, clickArgs) =>
                        fileIds.Values.Remove(fileId);

                    row.Add(removeButton, 0, 0);
                    row.Add(
                        FileIdEditor(
                            fileId,
                            nameof(Observable<FileId>.Value),
                            static (Observable<FileId> vm) => vm.Value,
                            static (vm, value) => vm.Value = value,
                            supportedType),
                        1,
                        0);
                };

                return row;
            }));

        var addButton = new Button { Text = "+" };
        addButton.Clicked += (sender, args) =>
            fileIds.Values.Add(new Observable<FileId>(new FileId()));

        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, args) => fileIds.Values.Clear();

        return new VerticalStackLayout
        {
            HorizontalOptions = LayoutOptions.Fill,
            Children =
            {
                new HorizontalStackLayout { Children = { addButton, clearButton } },
                listEditor
            }
        };
    }

    public static View InstanceIdEditor<TBindingContext>(object bindingContext, string bindingPath, Expression<Func<TBindingContext, InstanceId>> getter, Action<TBindingContext, InstanceId> setter, string expectedTypename = "")
        where TBindingContext : class
    {
        var valueEntry = CreateInspectorValueLabel();

        valueEntry.BindingContext = bindingContext;

        valueEntry.Bind<Label, TBindingContext, InstanceId, string>(Label.TextProperty,
            mode: BindingMode.TwoWay,
            converter: new InstanceIdToDisplayNameConverter(),
            getter: getter,
            setter: setter);

        var selectBehavior = new InstanceIdSelectBehavior();
        selectBehavior.SetBinding(InstanceIdSelectBehavior.BoundPropertyProperty, new Binding(bindingPath));

        var dragAndDropBehavior = new InstanceIdDragAndDropBehaviour
        {
            ExpectedTypename = expectedTypename ?? string.Empty
        };
        dragAndDropBehavior.SetBinding(InstanceIdDragAndDropBehaviour.BoundPropertyProperty, new Binding(bindingPath));

        valueEntry.Behaviors.Add(dragAndDropBehavior);
        valueEntry.Behaviors.Add(selectBehavior);

        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = GridLength.Star }
            }
        };

        grid.Add(valueEntry, 0, 0);

        return grid;
    }

    public static View Vec2Editor<TBindingContext>(Func<TBindingContext, Vec2> convert)
        where TBindingContext : class
    {
        return CreateInlineFieldGrid(
            FloatEditor((TBindingContext vm) => convert(vm).X, (TBindingContext vm, float value) => convert(vm).X = value),
            FloatEditor((TBindingContext vm) => convert(vm).Y, (TBindingContext vm, float value) => convert(vm).Y = value));
    }

    public static View Vec3Editor<TBindingContext>(Func<TBindingContext, Vec3> convert)
        where TBindingContext : class
    {
        return CreateInlineFieldGrid(
            FloatEditor((TBindingContext vm) => convert(vm).X, (TBindingContext vm, float value) => convert(vm).X = value),
            FloatEditor((TBindingContext vm) => convert(vm).Y, (TBindingContext vm, float value) => convert(vm).Y = value),
            FloatEditor((TBindingContext vm) => convert(vm).Z, (TBindingContext vm, float value) => convert(vm).Z = value));
    }

    public static View RotationEditor<TBindingContext>(Func<TBindingContext, Rotation> convert)
        where TBindingContext : class
    {
        return CreateInlineFieldGrid(
            FloatEditor((TBindingContext vm) => convert(vm).Yaw, (TBindingContext vm, float value) => convert(vm).Yaw = value),
            FloatEditor((TBindingContext vm) => convert(vm).Pitch, (TBindingContext vm, float value) => convert(vm).Pitch = value),
            FloatEditor((TBindingContext vm) => convert(vm).Roll, (TBindingContext vm, float value) => convert(vm).Roll = value));
    }

    public static View Vec4Editor<TBindingContext>(Func<TBindingContext, Vec4> convert)
        where TBindingContext : class
    {
        return CreateInlineFieldGrid(
            FloatEditor((TBindingContext vm) => convert(vm).X, (TBindingContext vm, float value) => convert(vm).X = value),
            FloatEditor((TBindingContext vm) => convert(vm).Y, (TBindingContext vm, float value) => convert(vm).Y = value),
            FloatEditor((TBindingContext vm) => convert(vm).Z, (TBindingContext vm, float value) => convert(vm).Z = value),
            FloatEditor((TBindingContext vm) => convert(vm).W, (TBindingContext vm, float value) => convert(vm).W = value));
    }

    public static View FloatEditor<TBindingContext>(Expression<Func<TBindingContext, float>> getter, Action<TBindingContext, float> setter)
        where TBindingContext : class
        => CreateFloatEditor(getter, setter, normalizeOnUnfocus: false);

    static Entry CreateFloatEditor<TBindingContext>(
        Expression<Func<TBindingContext, float>> getter,
        Action<TBindingContext, float> setter,
        bool normalizeOnUnfocus)
        where TBindingContext : class
    {
        var value = CreateInspectorEntry();
        value.Keyboard = Keyboard.Numeric;
        value.ReturnType = ReturnType.Done;
        value.IsTextPredictionEnabled = false;
        var getValue = getter.Compile();
        ConfigureCommittingEntry(
            value,
            normalizeOnUnfocus
                ? entry =>
                {
                    if (entry.BindingContext is TBindingContext bindingContext)
                        entry.Text = NumericRangeEntryInteraction.Format(getValue(bindingContext));
                }
                : null);

        value.Bind<Entry, TBindingContext, float, string>(Entry.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.TwoWay,
            converter: new FloatValueConverter());

        return value;
    }

    public static View RangedFloatEditor<TBindingContext>(
        Expression<Func<TBindingContext, float>> getter,
        Action<TBindingContext, float> setter,
        NumericPropertyRange range)
        where TBindingContext : class
    {
        var isSynchronizingPreview = false;
        var exactValueEditor = CreateFloatEditor(
            getter,
            (bindingContext, value) =>
            {
                if (!isSynchronizingPreview)
                    setter(bindingContext, range.Clamp(value));
            },
            normalizeOnUnfocus: true);

        return CreateRangedNumericEditor(
            CreateRangeSlider(
                getter,
                setter,
                range,
                value => (float)value,
                previewValue =>
                {
                    isSynchronizingPreview = true;
                    try
                    {
                        exactValueEditor.Text = NumericRangeEntryInteraction.Format(previewValue);
                    }
                    finally
                    {
                        isSynchronizingPreview = false;
                    }
                }),
            exactValueEditor);
    }

    public static View IntEditor<TBindingContext>(Expression<Func<TBindingContext, int>> getter, Action<TBindingContext, int> setter)
        where TBindingContext : class
        => CreateIntEditor(getter, setter, normalizeOnUnfocus: false);

    static Entry CreateIntEditor<TBindingContext>(
        Expression<Func<TBindingContext, int>> getter,
        Action<TBindingContext, int> setter,
        bool normalizeOnUnfocus)
        where TBindingContext : class
    {
        var value = CreateInspectorEntry();
        value.Keyboard = Keyboard.Numeric;
        value.ReturnType = ReturnType.Done;
        value.IsTextPredictionEnabled = false;
        var getValue = getter.Compile();
        ConfigureCommittingEntry(
            value,
            normalizeOnUnfocus
                ? entry =>
                {
                    if (entry.BindingContext is TBindingContext bindingContext)
                        entry.Text = NumericRangeEntryInteraction.Format(getValue(bindingContext));
                }
                : null);

        value.Bind<Entry, TBindingContext, int, string>(Entry.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.TwoWay,
            converter: new IntValueConverter());

        return value;
    }

    public static View RangedIntEditor<TBindingContext>(
        Expression<Func<TBindingContext, int>> getter,
        Action<TBindingContext, int> setter,
        NumericPropertyRange range)
        where TBindingContext : class
    {
        var isSynchronizingPreview = false;
        var exactValueEditor = CreateIntEditor(
            getter,
            (bindingContext, value) =>
            {
                if (!isSynchronizingPreview)
                    setter(bindingContext, range.Clamp(value));
            },
            normalizeOnUnfocus: true);

        return CreateRangedNumericEditor(
            CreateRangeSlider(
                getter,
                setter,
                range,
                range.SnapInt32,
                previewValue =>
                {
                    isSynchronizingPreview = true;
                    try
                    {
                        exactValueEditor.Text = NumericRangeEntryInteraction.Format(previewValue);
                    }
                    finally
                    {
                        isSynchronizingPreview = false;
                    }
                }),
            exactValueEditor);
    }

    public static View UIntEditor<TBindingContext>(Expression<Func<TBindingContext, uint>> getter, Action<TBindingContext, uint> setter)
        where TBindingContext : class
        => CreateUIntEditor(getter, setter, normalizeOnUnfocus: false);

    static Entry CreateUIntEditor<TBindingContext>(
        Expression<Func<TBindingContext, uint>> getter,
        Action<TBindingContext, uint> setter,
        bool normalizeOnUnfocus)
        where TBindingContext : class
    {
        var value = CreateInspectorEntry();
        value.Keyboard = Keyboard.Numeric;
        value.ReturnType = ReturnType.Done;
        value.IsTextPredictionEnabled = false;
        var getValue = getter.Compile();
        ConfigureCommittingEntry(
            value,
            normalizeOnUnfocus
                ? entry =>
                {
                    if (entry.BindingContext is TBindingContext bindingContext)
                        entry.Text = NumericRangeEntryInteraction.Format(getValue(bindingContext));
                }
                : null);

        value.Bind<Entry, TBindingContext, uint, string>(Entry.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.TwoWay,
            converter: new UIntValueConverter());

        return value;
    }

    public static View RangedUIntEditor<TBindingContext>(
        Expression<Func<TBindingContext, uint>> getter,
        Action<TBindingContext, uint> setter,
        NumericPropertyRange range)
        where TBindingContext : class
    {
        var isSynchronizingPreview = false;
        var exactValueEditor = CreateUIntEditor(
            getter,
            (bindingContext, value) =>
            {
                if (!isSynchronizingPreview)
                    setter(bindingContext, range.Clamp(value));
            },
            normalizeOnUnfocus: true);

        return CreateRangedNumericEditor(
            CreateRangeSlider(
                getter,
                setter,
                range,
                range.SnapUInt32,
                previewValue =>
                {
                    isSynchronizingPreview = true;
                    try
                    {
                        exactValueEditor.Text = NumericRangeEntryInteraction.Format(previewValue);
                    }
                    finally
                    {
                        isSynchronizingPreview = false;
                    }
                }),
            exactValueEditor);
    }

    static Slider CreateRangeSlider<TBindingContext, TValue>(
        Expression<Func<TBindingContext, TValue>> getter,
        Action<TBindingContext, TValue> setter,
        NumericPropertyRange range,
        Func<double, TValue> convertSliderValue,
        Action<TValue> previewValueChanged)
        where TBindingContext : class
    {
        var slider = new NumericRangeSlider(range);
        var getValue = getter.Compile();

        void ApplyDecision(NumericRangeSlider current, NumericRangeSliderDecision decision)
        {
            if (decision.ShouldPreview)
                previewValueChanged(convertSliderValue(decision.Value));

            if (decision.ShouldApply &&
                current.BindingContext is TBindingContext bindingContext)
            {
                setter(bindingContext, convertSliderValue(decision.Value));
                var actualValue = getValue(bindingContext);
                previewValueChanged(actualValue);
                current.SynchronizeFromModel(
                    System.Convert.ToDouble(
                        actualValue,
                        System.Globalization.CultureInfo.InvariantCulture));
            }
        }

        slider.Bind<NumericRangeSlider, TBindingContext, TValue, double>(
            NumericRangeSlider.ModelValueProperty,
            getter: getter,
            setter: static (_, _) => { },
            mode: BindingMode.OneWay,
            converter: new NumericRangeSliderConverter());

        slider.DragStarted += (sender, args) =>
        {
            var current = (NumericRangeSlider)sender;
            current.Interaction.BeginDrag(current.Value);
        };
        slider.ValueChanged += (sender, args) =>
        {
            var current = (NumericRangeSlider)sender;
            ApplyDecision(
                current,
                current.Interaction.HandleValueChanged(args.NewValue));
        };
        slider.DragCompleted += (sender, args) =>
        {
            var current = (NumericRangeSlider)sender;
            ApplyDecision(
                current,
                current.Interaction.EndDrag());
        };

        return slider;
    }

    static Grid CreateRangedNumericEditor(Slider slider, View exactValueEditor)
    {
        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = GridLength.Star },
                new ColumnDefinition { Width = new GridLength(96) }
            },
            ColumnSpacing = InspectorFieldSpacing,
            HorizontalOptions = LayoutOptions.Fill,
            MinimumWidthRequest = 0
        };

        grid.Add(slider, 0, 0);
        grid.Add(exactValueEditor, 1, 0);
        return grid;
    }

    sealed class NumericRangeSlider : Slider
    {
        public static readonly BindableProperty ModelValueProperty = BindableProperty.Create(
            nameof(ModelValue),
            typeof(double),
            typeof(NumericRangeSlider),
            0.0,
            BindingMode.OneWay,
            propertyChanged: OnModelValueChanged);

        public NumericRangeSlider(NumericPropertyRange range)
        {
            Interaction = new NumericRangeSliderInteraction(range);
            Minimum = range.Minimum;
            Maximum = range.Maximum;
            HorizontalOptions = LayoutOptions.Fill;
            MinimumWidthRequest = 0;
        }

        public NumericRangeSliderInteraction Interaction { get; }

        public double ModelValue
        {
            get => (double)GetValue(ModelValueProperty);
            set => SetValue(ModelValueProperty, value);
        }

        static void OnModelValueChanged(BindableObject bindable, object oldValue, object newValue)
        {
            var slider = (NumericRangeSlider)bindable;
            slider.SynchronizeFromModel((double)newValue);
        }

        public void SynchronizeFromModel(double modelValue)
        {
            var displayedValue = Interaction.BeginModelSynchronization(modelValue);
            try
            {
                Value = displayedValue;
            }
            finally
            {
                Interaction.EndModelSynchronization();
            }
        }
    }

    sealed class NumericRangeSliderConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => System.Convert.ToDouble(value, System.Globalization.CultureInfo.InvariantCulture);

        public object ConvertBack(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => BindableProperty.UnsetValue;
    }

    public static View StringEditor<TBindingContext>(Expression<Func<TBindingContext, string>> getter, Action<TBindingContext, string> setter)
        where TBindingContext : class
    {
        var value = CreateInspectorEntry();
        value.ReturnType = ReturnType.Done;
        ConfigureCommittingEntry(value);

        value.Bind<Entry, TBindingContext, string, string>(Entry.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.TwoWay,
            converter: (IValueConverter)null);

        return value;
    }

    static void ConfigureCommittingEntry(Entry entry, Action<Entry> normalizeOnUnfocus = null)
    {
        entry.Completed += (sender, args) =>
        {
            var current = (Entry)sender;
            current.Unfocus();
        };
        entry.Unfocused += (sender, args) =>
        {
            var current = (Entry)sender;
            normalizeOnUnfocus?.Invoke(current);
            ScheduleInspectorCommit(current);
        };
    }

    public static View UniformEditor<TBindingContext, T>(
        Expression<Func<TBindingContext, ObservableList<Uniform<T>>>> getter,
        Action<TBindingContext, ObservableList<Uniform<T>>> setter,
        Func<IView> valueEditor,
        string labelText,
        string defaultKey = default,
        T defaultValue = default)
    where TBindingContext : class, ICloneable, INotifyPropertyChanged
    where T : IComparable<T>
    {
        Func<TBindingContext, ObservableCollection<Uniform<T>>> dictGetter = getter.Compile();

        var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

        var dictEditor = new CollectionView();
        dictEditor.ItemTemplate = new DataTemplate(() =>
            {
                var keyEntry = new Entry();
                keyEntry.Bind(Entry.TextProperty,
                    getter: static (Uniform<T> vm) => vm.Key,
                    setter: static (Uniform<T> vm, string value) => vm.Key = value,
                    mode: BindingMode.TwoWay);

                var deleteButton = new Button { Text = "-" };
                deleteButton.Clicked += (sender, e) =>
                {
                    var dict = dictGetter((TBindingContext)dictEditor.BindingContext);

                    if ((sender as Button).BindingContext is Uniform<T> value)
                    {
                        dict.Remove(value);
                    }
                };

                var entryLayout = new HorizontalStackLayout { Children = { deleteButton, valueEditor(), keyEntry } };

                return entryLayout;
            });

        dictEditor.Bind(ItemsView.ItemsSourceProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay);

        var addButton = new Button { Text = "+" };
        addButton.Clicked += (sender, e) =>
        {
            var dict = dictGetter((TBindingContext)dictEditor.BindingContext);
            if (dict != null)
            {
                Uniform<T> v = new() { Key = defaultKey, Value = defaultValue };

                if (defaultValue is ICloneable cloneable)
                    v.Value = (T)cloneable.Clone();

                dict.Add(v);
            }
        };

        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, e) =>
        {
            var dict = dictGetter((TBindingContext)dictEditor.BindingContext);
            dict?.Clear();
        };

        var stackLayout = new VerticalStackLayout();
        stackLayout.Children.Add(label);
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
        stackLayout.Children.Add(dictEditor);

        return stackLayout;
    }

    public static View ListEditor<TBindingContext, T>(
        Expression<Func<TBindingContext, ObservableList<Observable<T>>>> getter,
        Action<TBindingContext, ObservableList<Observable<T>>> setter,
        Func<IView> valueEditor,
        string labelText,
        T defaultElement = default,
        IValueConverter converter = null)
    where TBindingContext : class, ICloneable, INotifyPropertyChanged
    where T : ICloneable, IComparable<T>
    {
        var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

        Func<TBindingContext, ObservableCollection<Observable<T>>> listGetter = getter.Compile();

        var listEditor = new CollectionView();
        listEditor.ItemTemplate = new DataTemplate(() =>
            {
                var deleteButton = new Button { Text = "-" };
                deleteButton.Clicked += (sender, e) =>
                {
                    var list = listGetter((TBindingContext)listEditor.BindingContext);
                    if ((sender as Button).BindingContext is Observable<T> value)
                    {
                        list.Remove(value);
                    }
                };

                var entryLayout = new HorizontalStackLayout { Children = { deleteButton, valueEditor() } };
                return entryLayout;
            });

        listEditor.Bind(ItemsView.ItemsSourceProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay);

        var addButton = new Button { Text = "+" };
        addButton.Clicked += (sender, e) =>
        {
            var list = listGetter((TBindingContext)listEditor.BindingContext);
            list?.Add(new Observable<T>(defaultElement));
        };

        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, e) =>
        {
            var list = listGetter((TBindingContext)listEditor.BindingContext);
            list?.Clear();
        };

        var stackLayout = new VerticalStackLayout();
        stackLayout.Children.Add(label);
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
        stackLayout.Children.Add(listEditor);

        return stackLayout;
    }

    public static ImageSource ResizeImageToThumbnail(string imagePath)
    {
        using var original = SKBitmap.Decode(imagePath);
        if (original == null)
        {
            return null;
        }

        int width = ThumbnailSize;
        int height = ThumbnailSize;

        float aspectRatio = original.Width / (float)original.Height;
        if (original.Width > original.Height)
        {
            height = (int)(width / aspectRatio);
        }
        else
        {
            width = (int)(height * aspectRatio);
        }

        using var resizedImage = original.Resize(new SKImageInfo(width, height), SKFilterQuality.High);

        if (resizedImage == null) return null;

        using var image = SKImage.FromBitmap(resizedImage);
        using var data = image.Encode(SKEncodedImageFormat.Png, 100);

        var stream = new MemoryStream();
        data.SaveTo(stream);
        stream.Seek(0, SeekOrigin.Begin);

        return ImageSource.FromStream(() => stream);
    }

    public static void AddGridRow(Microsoft.Maui.Controls.Grid grid, View view, Microsoft.Maui.GridLength gridLength)
    {
        grid.RowDefinitions.Add(new Microsoft.Maui.Controls.RowDefinition { Height = gridLength });
        grid.Children.Add(view);
        Grid.SetRow(view, grid.RowDefinitions.Count - 1);
    }

    public static void AddGridRowWithLabel(Grid grid, string labelText, View contentView, Microsoft.Maui.GridLength gridLength)
    {
        if (grid.ColumnDefinitions.Count == 0)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(InspectorLabelColumnWidth) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Star });
        }
        else if (grid.ColumnDefinitions.Count == 1)
        {
            grid.ColumnDefinitions[0].Width = new GridLength(InspectorLabelColumnWidth);
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Star });
        }

        grid.RowDefinitions.Add(new Microsoft.Maui.Controls.RowDefinition { Height = gridLength });

        var label = new Label
        {
            Text = labelText,
            VerticalOptions = LayoutOptions.Center,
            HorizontalOptions = LayoutOptions.Start,
            LineBreakMode = LineBreakMode.WordWrap,
            MaxLines = 2,
            Margin = new Thickness(0, 4, 0, 4)
        };

        if (contentView is View element)
        {
            element.HorizontalOptions = LayoutOptions.Fill;
            element.MinimumWidthRequest = 0;
            element.Margin = new Thickness(0, 2, 0, 2);
        }

        grid.Add(label, 0, grid.RowDefinitions.Count - 1);
        grid.Add(contentView, 1, grid.RowDefinitions.Count - 1);
    }
};
