using SailorEditor.Commands;
using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor.Utility;
using SailorEngine;
using System.Globalization;

namespace SailorEditor;

public sealed class AnimationControllerFileTemplate : DataTemplate
{
    public AnimationControllerFileTemplate()
    {
        LoadTemplate = static () => new AnimationControllerEditorPanel();
    }
}

public sealed class AnimationSetFileTemplate : DataTemplate
{
    public AnimationSetFileTemplate()
    {
        LoadTemplate = static () => new AnimationSetEditorPanel();
    }
}

sealed class AnimationControllerEditorPanel : VerticalStackLayout
{
    readonly ICommandDispatcher dispatcher;
    readonly IActionContextProvider contextProvider;
    AnimationControllerFile? controller;
    AnimationControllerGraphView? graph;
    ContentView? stateEditorHost;
    IDispatcherTimer? previewTimer;
    AnimationControllerState? selectedState;
    string? previewAnimatorInstanceId;
    int previewGeneration;
    bool rebuilding;

    public AnimationControllerEditorPanel()
    {
        Spacing = 10;
        dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        contextProvider = MauiProgram.GetService<IActionContextProvider>();
        BindingContextChanged += (_, _) => Bind(BindingContext as AnimationControllerFile);
    }

    void Bind(AnimationControllerFile? value)
    {
        if (!ReferenceEquals(controller, value))
        {
            previewAnimatorInstanceId = null;
        }
        if (controller is not null)
        {
            controller.DocumentReplaced -= OnDocumentReplaced;
        }
        controller = value;
        if (controller is not null)
        {
            controller.DocumentReplaced += OnDocumentReplaced;
        }
        Build();
    }

    void OnDocumentReplaced()
    {
        if (controller is null)
        {
            return;
        }
        if (selectedState is not null)
        {
            selectedState = controller.States.FirstOrDefault(state => state.Id == selectedState.Id);
        }
        Build();
    }

    void Build()
    {
        rebuilding = true;
        try
        {
            graph?.Detach();
            graph = null;
            stateEditorHost = null;
            previewTimer?.Stop();
            previewTimer = null;
            ++previewGeneration;
            Children.Clear();
            if (controller is null)
            {
                return;
            }

            Children.Add(new Views.ControlPanelView { BindingContext = controller });
            Children.Add(SectionLabel("State Machine"));

            var graphToolbar = new HorizontalStackLayout { Spacing = 6 };
            graphToolbar.Children.Add(Button("Add State", () =>
            {
                AnimationControllerState? state = null;
                ApplyMutation("Add animation state", () =>
                {
                    var offset = 24.0 * controller.States.Count;
                    state = controller.AddState(32.0 + offset, 32.0 + offset);
                });
                selectedState = state;
                Build();
            }));
            graphToolbar.Children.Add(Button("Set Default", () =>
            {
                if (selectedState is not null)
                {
                    ApplyMutation("Set default animation state", () => controller.SetDefaultState(selectedState));
                    Build();
                }
            }));
            graphToolbar.Children.Add(Button("Delete State", () =>
            {
                if (selectedState is not null)
                {
                    ApplyMutation("Delete animation state", () => controller.RemoveState(selectedState));
                    selectedState = null;
                    Build();
                }
            }));
            Children.Add(graphToolbar);

            graph = new AnimationControllerGraphView
            {
                BindingContext = controller,
                HeightRequest = 420,
                MinimumWidthRequest = 480,
                HorizontalOptions = LayoutOptions.Fill
            };
            graph.SelectState(selectedState);
            graph.StateSelected += state =>
            {
                selectedState = state;
                if (stateEditorHost is not null)
                {
                    stateEditorHost.Content = state is null ? null : BuildStateEditor(state);
                }
            };
            graph.DocumentEdited += DispatchDocumentEdit;
            var graphDrop = new DropGestureRecognizer();
            graphDrop.DragOver += (_, args) =>
            {
                args.AcceptedOperation = args.Data.Properties.TryGetValue(
                        EditorDragDrop.DragItemKey,
                        out var source) && source is AnimationFile
                    ? DataPackageOperation.Copy
                    : DataPackageOperation.None;
            };
            graphDrop.Drop += (_, args) =>
            {
                if (!args.Data.Properties.TryGetValue(
                        EditorDragDrop.DragItemKey,
                        out var source) || source is not AnimationFile animation)
                {
                    return;
                }

                args.Handled = true;
                var position = args.GetPosition(graph) ?? new Point(32.0, 32.0);
                AnimationControllerState? addedState = null;
                ApplyMutation("Create animation state from clip", () =>
                {
                    var slot = GetAnimationSlotName(animation);
                    addedState = controller.AddState(position.X, position.Y);
                    addedState.Name = GetUniqueStateName(slot);
                    addedState.Clip = slot;
                    selectedState = addedState;
                });
            };
            graph.GestureRecognizers.Add(graphDrop);
            Children.Add(graph);

            stateEditorHost = new ContentView
            {
                Content = selectedState is null ? null : BuildStateEditor(selectedState)
            };
            Children.Add(stateEditorHost);

            Children.Add(new Label
            {
                Text = "Drop an Animation asset onto the graph to create a clip state.",
                FontSize = 11,
                TextColor = Color.FromArgb("#929AA5")
            });

            Children.Add(BuildPreview());
            Children.Add(BuildParameters());
            Children.Add(BuildTransitions());
        }
        finally
        {
            rebuilding = false;
        }
    }

    View BuildStateEditor(AnimationControllerState state)
    {
        var panel = Card();
        panel.Children.Add(SectionLabel(
            state.Id == controller!.DefaultStateId
                ? $"{state.Name} (Default)"
                : state.Name));
        panel.Children.Add(Labeled("Name", TextEntry(state.Name, value =>
            ApplyMutation("Rename animation state", () => state.Name = value))));
        panel.Children.Add(Labeled("Clip slot", TextEntry(state.Clip, value =>
            ApplyMutation("Change animation clip slot", () => state.Clip = value))));
        panel.Children.Add(Labeled("Speed", FloatEntry(state.Speed, value =>
            ApplyMutation("Change animation state speed", () => state.Speed = value))));
        panel.Children.Add(Labeled("Loop", Check(state.Loop, value =>
            ApplyMutation("Change animation state loop", () => state.Loop = value))));
        return panel;
    }

    View BuildPreview()
    {
        var panel = Card();
        var header = new HorizontalStackLayout { Spacing = 8 };
        header.Children.Add(SectionLabel("Live Preview"));
        header.Children.Add(Button("Refresh Targets", Build));
        panel.Children.Add(header);
        var targets = FindPreviewTargets();
        var targetPicker = new Picker
        {
            Title = "Animator in the current scene",
            ItemsSource = targets,
            ItemDisplayBinding = new Binding(nameof(AnimationControllerPreviewTarget.DisplayName))
        };
        targetPicker.SelectedItem = targets.FirstOrDefault(target =>
                string.Equals(
                    target.Animator.InstanceId?.Value,
                    previewAnimatorInstanceId,
                    StringComparison.Ordinal)) ??
            targets.FirstOrDefault();
        panel.Children.Add(targetPicker);

        var stateLabel = new Label
        {
            Text = targets.Count == 0
                ? "Assign this controller and an Animation Set to an AnimatorComponent in the current scene."
                : "Waiting for runtime state...",
            FontSize = 11,
            TextColor = Color.FromArgb("#AAB2BE")
        };
        panel.Children.Add(stateLabel);

        var runtimeParameters = new ContentView();
        panel.Children.Add(runtimeParameters);
        panel.Children.Add(new Label
        {
            Text = "The selected Animator is evaluated by the Engine and rendered in Scene View. Runtime controls do not change authored parameter defaults.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5")
        });

        var timer = Dispatcher.CreateTimer();
        previewTimer = timer;
        timer.Interval = TimeSpan.FromMilliseconds(100);
        var generation = previewGeneration;
        var requestPending = false;

        async Task RefreshRuntimeStateAsync()
        {
            if (requestPending ||
                generation != previewGeneration ||
                targetPicker.SelectedItem is not AnimationControllerPreviewTarget target ||
                target.Animator.InstanceId is null ||
                target.Animator.InstanceId.IsEmpty())
            {
                return;
            }
            requestPending = true;
            try
            {
                var state = await MauiProgram.GetService<EngineService>()
                    .GetAnimatorStateAsync(target.Animator.InstanceId);
                if (generation != previewGeneration ||
                    !ReferenceEquals(targetPicker.SelectedItem, target))
                {
                    return;
                }
                if (state is null)
                {
                    stateLabel.Text = "Engine is not running.";
                    SetPreviewState(0);
                }
                else if (!state.Value.HasController)
                {
                    stateLabel.Text = "The runtime Animator has no valid controller/set.";
                    SetPreviewState(0);
                }
                else
                {
                    stateLabel.Text = state.Value.IsTransitioning
                        ? $"{state.Value.ActiveStateName} → {state.Value.DestinationStateName}  {state.Value.TransitionAlpha:P0}"
                        : $"{state.Value.ActiveStateName}  {state.Value.ActiveStateTime:F2}s";
                    SetPreviewState(state.Value.ActiveStateId);
                }
            }
            catch (Exception exception)
            {
                if (generation == previewGeneration)
                {
                    stateLabel.Text = exception.Message;
                    SetPreviewState(0);
                }
            }
            finally
            {
                requestPending = false;
            }
        }

        void BindTarget()
        {
            if (targetPicker.SelectedItem is AnimationControllerPreviewTarget target)
            {
                previewAnimatorInstanceId = target.Animator.InstanceId?.Value;
                runtimeParameters.Content = BuildRuntimeParameterControls(target, stateLabel);
                stateLabel.Text = target.HasAnimationSet
                    ? "Waiting for runtime state..."
                    : "The selected Animator has no Animation Set.";
            }
            else
            {
                previewAnimatorInstanceId = null;
                runtimeParameters.Content = null;
                stateLabel.Text = "Assign this controller and an Animation Set to an AnimatorComponent in the current scene.";
            }
            SetPreviewState(0);
            _ = RefreshRuntimeStateAsync();
        }

        targetPicker.SelectedIndexChanged += (_, _) => BindTarget();
        timer.Tick += (_, _) => _ = RefreshRuntimeStateAsync();
        panel.Loaded += (_, _) =>
        {
            if (ReferenceEquals(previewTimer, timer))
            {
                timer.Start();
                _ = RefreshRuntimeStateAsync();
            }
        };
        panel.Unloaded += (_, _) => timer.Stop();
        BindTarget();
        return panel;
    }

    List<AnimationControllerPreviewTarget> FindPreviewTargets()
    {
        var result = new List<AnimationControllerPreviewTarget>();
        if (controller?.FileId is null || controller.FileId.IsEmpty())
        {
            return result;
        }

        var worldService = MauiProgram.GetService<WorldService>();
        foreach (var prefab in worldService.Current.Prefabs)
        {
            foreach (var gameObject in prefab.GameObjects)
            {
                foreach (var component in worldService.GetComponents(gameObject))
                {
                    if (!string.Equals(
                            component.Typename?.Name,
                            "Sailor::AnimatorComponent",
                            StringComparison.Ordinal) ||
                        !AnimatorRuntimeControls.ReferencesAsset(
                            component,
                            "controller",
                            controller.FileId))
                    {
                        continue;
                    }

                    result.Add(new AnimationControllerPreviewTarget(
                        gameObject,
                        component,
                        AnimatorRuntimeControls.HasAssignedAsset(
                            component,
                            "animationSet")));
                }
            }
        }
        return result;
    }

    View BuildRuntimeParameterControls(
        AnimationControllerPreviewTarget target,
        Label statusLabel)
    {
        var parameters = new VerticalStackLayout { Spacing = 4 };
        if (!target.HasAnimationSet)
        {
            return parameters;
        }

        foreach (var parameter in controller!.Parameters)
        {
            var editor = AnimatorRuntimeControls.CreateParameterEditor(
                target.Animator,
                parameter,
                message => statusLabel.Text = message);
            parameters.Children.Add(Labeled(parameter.Name, editor));
        }
        return parameters;
    }

    void SetPreviewState(ulong stateId)
    {
        if (graph is null || graph.PreviewActiveStateId == stateId)
        {
            return;
        }
        graph.PreviewActiveStateId = stateId;
        graph.Invalidate();
    }

    sealed class AnimationControllerPreviewTarget(
        GameObject gameObject,
        Component animator,
        bool hasAnimationSet)
    {
        public GameObject GameObject { get; } = gameObject;
        public Component Animator { get; } = animator;
        public bool HasAnimationSet { get; } = hasAnimationSet;
        public string DisplayName { get; } = hasAnimationSet
            ? gameObject.Name
            : $"{gameObject.Name} (missing Animation Set)";
    }

    string GetUniqueStateName(string baseName)
    {
        var usedNames = controller!.States
            .Select(state => state.Name)
            .ToHashSet(StringComparer.Ordinal);
        if (!usedNames.Contains(baseName))
        {
            return baseName;
        }
        for (var index = 2; ; ++index)
        {
            var candidate = $"{baseName} {index}";
            if (!usedNames.Contains(candidate))
            {
                return candidate;
            }
        }
    }

    static string GetAnimationSlotName(AnimationFile animation)
    {
        var filename = animation.AssetInfo?.Name ?? animation.DisplayName ?? "Animation";
        if (filename.EndsWith(".asset", StringComparison.OrdinalIgnoreCase))
        {
            filename = Path.GetFileNameWithoutExtension(filename);
        }
        if (filename.EndsWith(".anim", StringComparison.OrdinalIgnoreCase))
        {
            filename = Path.GetFileNameWithoutExtension(filename);
        }
        var separator = filename.LastIndexOf('_');
        return separator >= 0 && separator + 1 < filename.Length
            ? filename[(separator + 1)..]
            : filename;
    }

    View BuildParameters()
    {
        var panel = Card();
        var header = new HorizontalStackLayout { Spacing = 8 };
        header.Children.Add(SectionLabel("Parameters"));
        header.Children.Add(Button("+", () =>
        {
            ApplyMutation("Add animation parameter", () => controller!.AddParameter());
            Build();
        }));
        panel.Children.Add(header);

        foreach (var parameter in controller!.Parameters)
        {
            var row = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = 28 },
                    new ColumnDefinition { Width = GridLength.Star },
                    new ColumnDefinition { Width = 100 },
                    new ColumnDefinition { Width = 100 }
                },
                ColumnSpacing = 5
            };
            var remove = Button("-", () =>
            {
                ApplyMutation("Delete animation parameter", () => controller.RemoveParameter(parameter));
                Build();
            });
            row.Add(remove, 0);
            row.Add(TextEntry(parameter.Name, value =>
                ApplyMutation("Rename animation parameter", () => parameter.Name = value)), 1);
            var type = EnumPicker(parameter.Type, value =>
                ApplyMutation("Change animation parameter type", () => parameter.Type = value));
            row.Add(type, 2);
            row.Add(ParameterDefaultEditor(parameter), 3);
            panel.Children.Add(row);
        }
        return panel;
    }

    View ParameterDefaultEditor(AnimationControllerParameter parameter) => parameter.Type switch
    {
        AnimationControllerParameterType.Float => FloatEntry(parameter.DefaultFloat, value =>
            ApplyMutation("Change animation parameter default", () => parameter.DefaultFloat = value)),
        AnimationControllerParameterType.Int => IntEntry(parameter.DefaultInt, value =>
            ApplyMutation("Change animation parameter default", () => parameter.DefaultInt = value)),
        AnimationControllerParameterType.Bool => Check(parameter.DefaultBool, value =>
            ApplyMutation("Change animation parameter default", () => parameter.DefaultBool = value)),
        _ => new Label { Text = "one-shot", VerticalTextAlignment = TextAlignment.Center }
    };

    View BuildTransitions()
    {
        var panel = Card();
        panel.Children.Add(SectionLabel("Transitions (priority, then authored order)"));
        var from = StatePicker(controller!.States.FirstOrDefault());
        var to = StatePicker(controller.States.Skip(1).FirstOrDefault() ?? controller.States.FirstOrDefault());
        var add = Button("Add Transition", () =>
        {
            if (from.SelectedItem is AnimationControllerState fromState &&
                to.SelectedItem is AnimationControllerState toState)
            {
                ApplyMutation("Add animation transition", () => controller.AddTransition(fromState, toState));
                Build();
            }
        });
        panel.Children.Add(new HorizontalStackLayout
        {
            Spacing = 5,
            Children = { from, new Label { Text = "→", VerticalTextAlignment = TextAlignment.Center }, to, add }
        });

        foreach (var transition in controller.Transitions)
        {
            panel.Children.Add(BuildTransition(transition));
        }
        return panel;
    }

    View BuildTransition(AnimationControllerTransition transition)
    {
        var panel = Card(Color.FromArgb("#20252C"));
        var from = controller!.States.FirstOrDefault(state => state.Id == transition.FromStateId);
        var to = controller.States.FirstOrDefault(state => state.Id == transition.ToStateId);
        var header = new HorizontalStackLayout { Spacing = 7 };
        header.Children.Add(new Label
        {
            Text = $"{from?.Name ?? "Missing"} → {to?.Name ?? "Missing"}",
            FontAttributes = FontAttributes.Bold,
            VerticalTextAlignment = TextAlignment.Center
        });
        header.Children.Add(Button("Delete", () =>
        {
            ApplyMutation("Delete animation transition", () => controller.RemoveTransition(transition));
            Build();
        }));
        panel.Children.Add(header);
        panel.Children.Add(Labeled("Priority", IntEntry(transition.Priority, value =>
            ApplyMutation("Change transition priority", () => transition.Priority = value))));
        panel.Children.Add(Labeled("Crossfade seconds", FloatEntry(transition.Duration, value =>
            ApplyMutation("Change transition duration", () => transition.Duration = value))));
        panel.Children.Add(Labeled("Has exit time", Check(transition.HasExitTime, value =>
            ApplyMutation("Change transition exit time mode", () => transition.HasExitTime = value))));
        panel.Children.Add(Labeled("Normalized exit", FloatEntry(transition.ExitTime, value =>
            ApplyMutation("Change transition exit time", () => transition.ExitTime = value))));

        var addCondition = Button("Add AND Condition", () =>
        {
            var parameter = controller.Parameters.FirstOrDefault();
            if (parameter is not null)
            {
                ApplyMutation("Add transition condition", () => controller.AddCondition(transition, parameter));
                Build();
            }
        });
        addCondition.IsEnabled = controller.Parameters.Count > 0;
        panel.Children.Add(addCondition);
        foreach (var condition in transition.Conditions)
        {
            panel.Children.Add(BuildCondition(transition, condition));
        }
        return panel;
    }

    View BuildCondition(
        AnimationControllerTransition transition,
        AnimationControllerCondition condition)
    {
        var row = new HorizontalStackLayout { Spacing = 5 };
        row.Children.Add(Button("-", () =>
        {
            ApplyMutation("Delete transition condition", () => transition.Conditions.Remove(condition));
            Build();
        }));
        var parameterPicker = new Picker
        {
            WidthRequest = 120,
            ItemsSource = controller!.Parameters,
            ItemDisplayBinding = new Binding(nameof(AnimationControllerParameter.Name)),
            SelectedItem = controller.Parameters.FirstOrDefault(parameter => parameter.Id == condition.ParameterId)
        };
        parameterPicker.SelectedIndexChanged += (_, _) =>
        {
            if (!rebuilding && parameterPicker.SelectedItem is AnimationControllerParameter parameter)
            {
                ApplyMutation("Change transition condition parameter", () =>
                {
                    condition.ParameterId = parameter.Id;
                    condition.Operation = parameter.Type == AnimationControllerParameterType.Trigger
                        ? AnimationControllerConditionOperation.IsSet
                        : AnimationControllerConditionOperation.Equal;
                });
                Build();
            }
        };
        row.Children.Add(parameterPicker);
        row.Children.Add(EnumPicker(condition.Operation, value =>
            ApplyMutation("Change transition condition operation", () => condition.Operation = value)));

        var parameterType = (parameterPicker.SelectedItem as AnimationControllerParameter)?.Type;
        row.Children.Add(parameterType switch
        {
            AnimationControllerParameterType.Float => FloatEntry(condition.FloatValue, value =>
                ApplyMutation("Change transition condition value", () => condition.FloatValue = value)),
            AnimationControllerParameterType.Int => IntEntry(condition.IntValue, value =>
                ApplyMutation("Change transition condition value", () => condition.IntValue = value)),
            AnimationControllerParameterType.Bool => Check(condition.BoolValue, value =>
                ApplyMutation("Change transition condition value", () => condition.BoolValue = value)),
            _ => new Label { Text = "set", VerticalTextAlignment = TextAlignment.Center }
        });
        return row;
    }

    void ApplyMutation(string description, Action mutation)
    {
        if (controller is null)
        {
            return;
        }
        var before = controller.CaptureDocument();
        mutation();
        controller.ValidateDocument();
        var after = controller.CaptureDocument();
        if (!string.Equals(before, after, StringComparison.Ordinal))
        {
            DispatchDocumentEdit(before, after, description);
        }
    }

    async void DispatchDocumentEdit(string before, string after, string description)
    {
        if (controller is null ||
            string.Equals(before, after, StringComparison.Ordinal))
        {
            return;
        }
        try
        {
            var result = await dispatcher.DispatchAsync(
                new EditAnimationControllerCommand(controller, before, after, description),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(CommandOriginKind.Panel, nameof(AnimationControllerEditorPanel))));
            if (!result.Succeeded)
            {
                controller.ApplyDocument(before);
                MauiProgram.GetService<EngineService>().PushConsoleMessage(
                    result.Message ?? $"{description} failed.");
            }
        }
        catch (Exception exception)
        {
            controller.ApplyDocument(before);
            MauiProgram.GetService<EngineService>().PushConsoleMessage(exception.Message);
        }
    }

    Picker StatePicker(AnimationControllerState? selected) => new()
    {
        WidthRequest = 120,
        ItemsSource = controller!.States,
        ItemDisplayBinding = new Binding(nameof(AnimationControllerState.Name)),
        SelectedItem = selected
    };

    static Picker EnumPicker<T>(T selected, Action<T> changed)
        where T : struct, Enum
    {
        var picker = new Picker
        {
            WidthRequest = 118,
            ItemsSource = Enum.GetValues<T>(),
            SelectedItem = selected
        };
        picker.SelectedIndexChanged += (_, _) =>
        {
            if (picker.SelectedItem is T value && !EqualityComparer<T>.Default.Equals(value, selected))
            {
                changed(value);
            }
        };
        return picker;
    }

    static Entry TextEntry(string value, Action<string> changed)
    {
        var entry = new Entry { Text = value ?? string.Empty, ReturnType = ReturnType.Done };
        entry.Unfocused += (_, _) =>
        {
            if (!string.Equals(entry.Text, value, StringComparison.Ordinal))
            {
                changed(entry.Text ?? string.Empty);
            }
        };
        entry.Completed += (_, _) => entry.Unfocus();
        return entry;
    }

    static Entry FloatEntry(float value, Action<float> changed)
    {
        var entry = new Entry
        {
            Text = value.ToString("R", CultureInfo.InvariantCulture),
            Keyboard = Keyboard.Numeric,
            WidthRequest = 100,
            ReturnType = ReturnType.Done
        };
        entry.Unfocused += (_, _) =>
        {
            if (float.TryParse(entry.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) && parsed != value)
            {
                changed(parsed);
            }
        };
        entry.Completed += (_, _) => entry.Unfocus();
        return entry;
    }

    static Entry IntEntry(int value, Action<int> changed)
    {
        var entry = new Entry
        {
            Text = value.ToString(CultureInfo.InvariantCulture),
            Keyboard = Keyboard.Numeric,
            WidthRequest = 90,
            ReturnType = ReturnType.Done
        };
        entry.Unfocused += (_, _) =>
        {
            if (int.TryParse(entry.Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) && parsed != value)
            {
                changed(parsed);
            }
        };
        entry.Completed += (_, _) => entry.Unfocus();
        return entry;
    }

    static CheckBox Check(bool value, Action<bool> changed)
    {
        var check = new CheckBox { IsChecked = value };
        check.CheckedChanged += (_, args) =>
        {
            if (args.Value != value)
            {
                changed(args.Value);
            }
        };
        return check;
    }

    static Button Button(string text, Action clicked)
    {
        var button = new Button { Text = text, HeightRequest = 32, Padding = new Thickness(8, 2) };
        button.Clicked += (_, _) => clicked();
        return button;
    }

    static Label SectionLabel(string text) => new()
    {
        Text = text,
        FontAttributes = FontAttributes.Bold,
        VerticalTextAlignment = TextAlignment.Center
    };

    static View Labeled(string label, View editor) => new Grid
    {
        ColumnDefinitions =
        {
            new ColumnDefinition { Width = 150 },
            new ColumnDefinition { Width = GridLength.Star }
        },
        Children =
        {
            new Label { Text = label, VerticalTextAlignment = TextAlignment.Center },
            editor
        }
    }.Apply(grid => Grid.SetColumn(editor, 1));

    static VerticalStackLayout Card(Color? color = null) => new()
    {
        Spacing = 6,
        Padding = 8,
        BackgroundColor = color ?? Color.FromArgb("#1D2229")
    };
}

sealed class AnimationSetEditorPanel : VerticalStackLayout
{
    AnimationSetFile? set;

    public AnimationSetEditorPanel()
    {
        Spacing = 8;
        BindingContextChanged += (_, _) =>
        {
            set = BindingContext as AnimationSetFile;
            Build();
        };
    }

    void Build()
    {
        Children.Clear();
        if (set is null)
        {
            return;
        }
        Children.Add(new Views.ControlPanelView { BindingContext = set });
        Children.Add(new Label { Text = "Clip Slots", FontAttributes = FontAttributes.Bold });
        var add = new Button { Text = "Add Slot" };
        add.Clicked += (_, _) =>
        {
            set.AddClip();
            Build();
        };
        Children.Add(add);
        foreach (var clip in set.Clips)
        {
            var row = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = 28 },
                    new ColumnDefinition { Width = 140 },
                    new ColumnDefinition { Width = GridLength.Star }
                },
                ColumnSpacing = 5
            };
            var remove = new Button { Text = "-" };
            remove.Clicked += (_, _) =>
            {
                set.Clips.Remove(clip);
                Build();
            };
            var slot = new Entry { Text = clip.Slot, Placeholder = "logical slot" };
            slot.TextChanged += (_, args) => clip.Slot = args.NewTextValue ?? string.Empty;
            var animation = Templates.FileIdEditor(
                clip,
                nameof(AnimationSetClip.Animation),
                static (AnimationSetClip value) => value.Animation,
                static (value, fileId) => value.Animation = fileId,
                typeof(AnimationFile));
            row.Add(remove, 0);
            row.Add(slot, 1);
            row.Add(animation, 2);
            Children.Add(row);
        }
    }
}

static class AnimationControllerEditorViewExtensions
{
    public static T Apply<T>(this T value, Action<T> action)
    {
        action(value);
        return value;
    }
}
