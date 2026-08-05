using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Globalization;

namespace SailorEditor;

static class AnimatorRuntimeControls
{
    public static AnimationControllerFile? ResolveController(Component component)
    {
        if (!TryGetAssignedFileId(component, "controller", out var controllerId))
        {
            return null;
        }
        return MauiProgram.GetService<AssetsService>().Assets
            .TryGetValue(controllerId, out var asset)
            ? asset as AnimationControllerFile
            : null;
    }

    public static bool ReferencesAsset(
        Component component,
        string propertyName,
        FileId fileId) =>
        TryGetAssignedFileId(component, propertyName, out var assignedId) &&
        string.Equals(assignedId.Value, fileId.Value, StringComparison.Ordinal);

    public static bool HasAssignedAsset(
        Component component,
        string propertyName) =>
        TryGetAssignedFileId(component, propertyName, out _);

    public static View CreateParameterEditor(
        Component component,
        AnimationControllerParameter parameter,
        Action<string>? reportError = null) => parameter.Type switch
    {
        AnimationControllerParameterType.Float =>
            CreateFloatEditor(component, parameter, reportError),
        AnimationControllerParameterType.Int =>
            CreateIntEditor(component, parameter, reportError),
        AnimationControllerParameterType.Bool =>
            CreateBoolEditor(component, parameter, reportError),
        AnimationControllerParameterType.Trigger =>
            CreateTriggerEditor(component, parameter, reportError),
        _ => new Label { Text = "Unsupported parameter" }
    };

    static View CreateFloatEditor(
        Component component,
        AnimationControllerParameter parameter,
        Action<string>? reportError)
    {
        var entry = new Entry
        {
            Text = parameter.DefaultFloat.ToString("R", CultureInfo.InvariantCulture),
            Keyboard = Keyboard.Numeric,
            ReturnType = ReturnType.Done
        };
        entry.Completed += (_, _) => entry.Unfocus();
        entry.Unfocused += (_, _) =>
        {
            if (float.TryParse(
                    entry.Text,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var value))
            {
                RunUpdate(
                    component,
                    parameter.Name,
                    instanceId => MauiProgram.GetService<EngineService>()
                        .SetAnimatorFloatAsync(instanceId, parameter.Name, value),
                    reportError);
            }
        };
        return entry;
    }

    static View CreateIntEditor(
        Component component,
        AnimationControllerParameter parameter,
        Action<string>? reportError)
    {
        var entry = new Entry
        {
            Text = parameter.DefaultInt.ToString(CultureInfo.InvariantCulture),
            Keyboard = Keyboard.Numeric,
            ReturnType = ReturnType.Done
        };
        entry.Completed += (_, _) => entry.Unfocus();
        entry.Unfocused += (_, _) =>
        {
            if (int.TryParse(
                    entry.Text,
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out var value))
            {
                RunUpdate(
                    component,
                    parameter.Name,
                    instanceId => MauiProgram.GetService<EngineService>()
                        .SetAnimatorIntAsync(instanceId, parameter.Name, value),
                    reportError);
            }
        };
        return entry;
    }

    static View CreateBoolEditor(
        Component component,
        AnimationControllerParameter parameter,
        Action<string>? reportError)
    {
        var check = new CheckBox { IsChecked = parameter.DefaultBool };
        check.CheckedChanged += (_, args) => RunUpdate(
            component,
            parameter.Name,
            instanceId => MauiProgram.GetService<EngineService>()
                .SetAnimatorBoolAsync(instanceId, parameter.Name, args.Value),
            reportError);
        return check;
    }

    static View CreateTriggerEditor(
        Component component,
        AnimationControllerParameter parameter,
        Action<string>? reportError)
    {
        var fire = new Button { Text = "Fire", HeightRequest = 30 };
        fire.Clicked += (_, _) => RunUpdate(
            component,
            parameter.Name,
            instanceId => MauiProgram.GetService<EngineService>()
                .SetAnimatorTriggerAsync(instanceId, parameter.Name),
            reportError);
        return fire;
    }

    static void RunUpdate(
        Component component,
        string parameterName,
        Func<InstanceId, Task<bool>> update,
        Action<string>? reportError)
    {
        _ = RunUpdateAsync(component, parameterName, update, reportError);
    }

    static async Task RunUpdateAsync(
        Component component,
        string parameterName,
        Func<InstanceId, Task<bool>> update,
        Action<string>? reportError)
    {
        try
        {
            if (component.InstanceId is null || component.InstanceId.IsEmpty() ||
                !await update(component.InstanceId))
            {
                ReportError(
                    $"Runtime rejected parameter '{parameterName}'.",
                    reportError);
            }
        }
        catch (Exception exception)
        {
            ReportError(exception.Message, reportError);
        }
    }

    static void ReportError(string message, Action<string>? reportError)
    {
        if (reportError is not null)
        {
            reportError(message);
        }
        else
        {
            MauiProgram.GetService<EngineService>().PushConsoleMessage(message);
        }
    }

    static bool TryGetAssignedFileId(
        Component component,
        string propertyName,
        out FileId fileId)
    {
        if (component.OverrideProperties.TryGetValue(propertyName, out var value) &&
            value is ObjectPtr pointer &&
            pointer.FileId is not null &&
            !pointer.FileId.IsEmpty())
        {
            fileId = pointer.FileId;
            return true;
        }
        fileId = null!;
        return false;
    }
}
