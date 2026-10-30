namespace SailorEditor.Commands;

public static class EditorCommandBatch
{
    public static IEnumerable<T> ResultValues<T>(CommandResult result)
    {
        if (result.Value is T value)
        {
            yield return value;
        }
        else if (result.Value is IReadOnlyList<CommandResult> children)
        {
            foreach (var child in children.Where(child => child.Succeeded))
            {
                foreach (var childValue in ResultValues<T>(child))
                {
                    yield return childValue;
                }
            }
        }
    }

    public static IEditorCommand Create(string description, IEnumerable<IEditorCommand> commands)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(description);
        ArgumentNullException.ThrowIfNull(commands);
        var snapshot = commands.ToArray();
        if (snapshot.Length == 0 || snapshot.Any(command => command is null))
        {
            throw new ArgumentException("A command batch must contain at least one command.", nameof(commands));
        }

        if (snapshot.Length == 1)
        {
            return snapshot[0];
        }

        return snapshot.All(command => command is IUndoableEditorCommand)
            ? new CompoundEditorCommand(description, snapshot.Cast<IUndoableEditorCommand>().ToArray())
            : new SequentialEditorCommand(description, snapshot);
    }

    sealed class SequentialEditorCommand(string description, IReadOnlyList<IEditorCommand> commands) : IEditorCommand
    {
        public string Name => description;

        public bool CanExecute(ActionContext context) => commands.All(command => command.CanExecute(context));

        public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            if (!CanExecute(context))
            {
                return CommandResult.Failure($"{description}: one or more selected items cannot be changed.");
            }

            var results = new List<CommandResult>(commands.Count);
            for (var index = 0; index < commands.Count; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var command = commands[index];
                CommandResult result;
                try
                {
                    result = command.CanExecute(context)
                        ? await command.ExecuteAsync(context, cancellationToken)
                        : CommandResult.Failure("The item is no longer available for this operation.");
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (Exception exception)
                {
                    result = CommandResult.Failure(exception.Message);
                }

                results.Add(result);
                if (!result.Succeeded)
                {
                    return CommandResult.Failure(
                        $"{description}: completed {index} of {commands.Count} items; '{command.Name}' failed. {result.Message}",
                        results.AsReadOnly());
                }
            }

            return CommandResult.Success(value: results.AsReadOnly());
        }
    }
}
