using SailorEditor.Workspace;

namespace SailorEditor.Services;

public static class ProbeVolumeBakeOutputPolicy
{
    public const string Extension = ".probes";

    public static string FindAvailableStateName(
        string prefix,
        IEnumerable<string> reservedStateNames,
        Func<string, bool> outputExists)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(prefix);
        ArgumentNullException.ThrowIfNull(reservedStateNames);
        ArgumentNullException.ThrowIfNull(outputExists);

        var reserved = reservedStateNames.ToHashSet(StringComparer.Ordinal);
        for (var suffix = 1; suffix <= 10_000; ++suffix)
        {
            var candidate = suffix == 1 ? prefix : $"{prefix}{suffix}";
            if (!reserved.Contains(candidate) && !outputExists(candidate))
                return candidate;
        }

        throw new InvalidOperationException(
            $"No available probe-volume state name starts with '{prefix}'.");
    }

    public static bool TryResolveWriteTarget(
        string activeContentRoot,
        string outputVirtualPath,
        bool overwrite,
        out string physicalPath,
        out string error)
    {
        physicalPath = string.Empty;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(activeContentRoot) ||
            string.IsNullOrWhiteSpace(outputVirtualPath))
        {
            error = "A Content-relative bake output is required.";
            return false;
        }

        var candidate = outputVirtualPath.Trim();
        if (!string.Equals(
                Path.GetExtension(candidate),
                Extension,
                StringComparison.OrdinalIgnoreCase))
        {
            error = "The bake output must use the .probes extension.";
            return false;
        }

        try
        {
            physicalPath = WorkspacePathPolicy.ResolveOwnedPath(
                activeContentRoot,
                candidate,
                "Bake output");
        }
        catch (Exception exception)
        {
            error = exception.Message;
            physicalPath = string.Empty;
            return false;
        }

        if (!overwrite &&
            (File.Exists(physicalPath) || File.Exists(physicalPath + ".asset")))
        {
            error =
                $"The bake output '{candidate}' already exists. " +
                "Enable 'Overwrite existing file' or choose another output.";
            physicalPath = string.Empty;
            return false;
        }

        return true;
    }
}
