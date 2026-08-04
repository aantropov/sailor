using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.ViewModels;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.Core.Tokens;
using System.Numerics;
using SailorEditor.Content;

namespace SailorEditor.Services
{
    public partial class WorldService : ObservableObject
    {
        readonly EngineService engineService;
        readonly AssetsService assetsService;
        Action<string> worldUpdateHandler = delegate { };
        long workspaceEpoch;

        class WorldCache
        {
            public Dictionary<InstanceId, Component> Components { get; } = new();
            public Dictionary<InstanceId, GameObject> GameObjects { get; } = new();
            public Dictionary<InstanceId, GameObject> ComponentOwners { get; } = new();
        }

        public World Current { get; private set; } = new World();
        public WorldFile? CurrentWorldAsset { get; private set; }
        public bool IsCurrentWorldUntitled { get; private set; }
        public long WorkspaceEpoch => Interlocked.Read(ref workspaceEpoch);

        [ObservableProperty]
        ObservableList<ObservableList<GameObject>> gameObjects = new();

        public event Action<World> OnUpdateWorldAction = delegate { };

        public WorldService()
        {
            engineService = MauiProgram.GetService<EngineService>();
            assetsService = MauiProgram.GetService<AssetsService>();
            assetsService.Changed += RebindCurrentWorldAsset;
            SubscribeToWorldUpdates();
        }

        void RebindCurrentWorldAsset()
        {
            var currentWorldAsset = CurrentWorldAsset;
            var hasStableFileId = currentWorldAsset?.FileId is not null && !currentWorldAsset.FileId.IsEmpty();
            WorldFile? refreshedWorld = null;

            if (hasStableFileId &&
                assetsService.Assets.TryGetValue(currentWorldAsset!.FileId, out var refreshedAsset) &&
                refreshedAsset is WorldFile candidate)
            {
                refreshedWorld = candidate;
            }

            var result = WorldAssetRebindPolicy.Resolve(
                currentWorldAsset,
                hasStableFileId,
                refreshedWorld,
                IsCurrentWorldUntitled,
                currentWorldAsset?.IsDirty ?? false);
            if (result.Asset is not null && result.DirtyState is bool dirtyState)
            {
                result.Asset.IsDirty = dirtyState;
            }

            CurrentWorldAsset = result.Asset;
            IsCurrentWorldUntitled = result.IsUntitled;
        }

        void SubscribeToWorldUpdates()
        {
            var subscribedEpoch = WorkspaceEpoch;
            worldUpdateHandler = yaml => TryPopulateWorld(yaml, subscribedEpoch);
            engineService.OnUpdateCurrentWorldAction += worldUpdateHandler;
        }

        public void ResetForWorkspaceChange()
        {
            Interlocked.Increment(ref workspaceEpoch);
            engineService.OnUpdateCurrentWorldAction -= worldUpdateHandler;

            worldCaches.Clear();
            currentCache = new WorldCache();
            Current = new World();
            CurrentWorldAsset = null;
            IsCurrentWorldUntitled = false;
            GameObjects.Clear();

            SubscribeToWorldUpdates();
            OnUpdateWorldAction?.Invoke(Current);
        }

        public void MarkCurrentWorldUntitledForWorkspaceStartup()
        {
            CurrentWorldAsset = null;
            IsCurrentWorldUntitled = true;
        }

        static string GetWorldKey(World world) => string.IsNullOrEmpty(world?.Name) ? "__default_world__" : world.Name;

        public Component GetComponent(InstanceId instanceId) => componentsDict[instanceId];
        public GameObject GetGameObject(InstanceId instanceId) => gameObjectsDict[instanceId];
        public bool TryGetComponent(InstanceId instanceId, out Component component) => componentsDict.TryGetValue(instanceId, out component);
        public bool TryGetGameObject(InstanceId instanceId, out GameObject gameObject) => gameObjectsDict.TryGetValue(instanceId, out gameObject);

        public InstanceId? ResolveParentInstanceId(GameObject gameObject)
        {
            if (gameObject is null ||
                gameObject.PrefabIndex < 0 ||
                gameObject.PrefabIndex >= Current.Prefabs.Count)
            {
                return null;
            }

            var prefab = Current.Prefabs[gameObject.PrefabIndex];
            if (gameObject.ParentIndex != uint.MaxValue)
            {
                if (gameObject.ParentIndex >= prefab.GameObjects.Count)
                {
                    return null;
                }

                var parentInstanceId =
                    prefab.GameObjects[(int)gameObject.ParentIndex].InstanceId;
                return parentInstanceId is null || parentInstanceId.IsEmpty()
                    ? null
                    : new InstanceId(parentInstanceId.Value);
            }

            return string.IsNullOrWhiteSpace(prefab.ParentInstanceId) ||
                string.Equals(
                    prefab.ParentInstanceId,
                    InstanceId.NullInstanceId,
                    StringComparison.Ordinal)
                ? null
                : new InstanceId(prefab.ParentInstanceId);
        }

        public List<Component> GetComponents(GameObject gameObject)
        {
            List<Component> res = [];
            if (gameObject?.InstanceId is not null &&
                !gameObject.InstanceId.IsEmpty() &&
                gameObjectsDict.TryGetValue(gameObject.InstanceId, out var projectedGameObject))
            {
                gameObject = projectedGameObject;
            }

            if (gameObject is null ||
                gameObject.PrefabIndex < 0 ||
                gameObject.PrefabIndex >= Current.Prefabs.Count)
            {
                return res;
            }

            var prefab = Current.Prefabs[gameObject.PrefabIndex];
            foreach (var componentIndex in gameObject.ComponentIndices ?? [])
            {
                if (componentIndex < 0 || componentIndex >= prefab.Components.Count)
                    continue;

                res.Add(prefab.Components[componentIndex]);
            }

            return res;
        }

        public Component FindComponent(GameObject gameObject, string componentTypeName)
        {
            return GetComponents(gameObject).FirstOrDefault(component => component.Typename.Name == componentTypeName);
        }

        public GameObject FindOwner(Component component)
        {
            if (component?.InstanceId is null || component.InstanceId.IsEmpty())
            {
                return null;
            }

            return componentOwnersDict.TryGetValue(component.InstanceId, out var owner) ? owner : null;
        }

        public async Task<bool> CreateGameObjectAsync(
            GameObject? parent = null,
            CancellationToken cancellationToken = default)
        {
            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new CreateGameObjectCommand(parent?.InstanceId),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(CreateGameObjectAsync))),
                cancellationToken);
            return result.Succeeded;
        }

        public async Task<bool> RemoveGameObjectAsync(
            GameObject gameObject,
            CancellationToken cancellationToken = default)
        {
            if (gameObject == null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new DestroyGameObjectCommand(gameObject),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(RemoveGameObjectAsync))),
                cancellationToken);
            if (result.Succeeded)
            {
                MauiProgram.GetService<SelectionService>().ClearSelection();
            }

            return result.Succeeded;
        }

        public async Task<bool> ResetComponentToDefaultsAsync(
            Component component,
            CancellationToken cancellationToken = default)
        {
            if (component == null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new ResetComponentToDefaultsCommand(component),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(ResetComponentToDefaultsAsync))),
                cancellationToken);
            return result.Succeeded;
        }

        public async Task<bool> AddComponentAsync(
            GameObject gameObject,
            string componentTypeName,
            CancellationToken cancellationToken = default)
        {
            if (gameObject == null || string.IsNullOrWhiteSpace(componentTypeName))
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new AddComponentCommand(gameObject, componentTypeName),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(AddComponentAsync))),
                cancellationToken);
            return result.Succeeded;
        }

        public async Task<bool> RemoveComponentAsync(
            Component component,
            CancellationToken cancellationToken = default)
        {
            if (component == null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new RemoveComponentCommand(component),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(RemoveComponentAsync))),
                cancellationToken);
            if (result.Succeeded && ReferenceEquals(MauiProgram.GetService<SelectionService>().SelectedItem, component))
            {
                MauiProgram.GetService<SelectionService>().ClearSelection();
            }

            return result.Succeeded;
        }

        public async Task<bool> RenameGameObjectAsync(
            GameObject gameObject,
            string newName,
            CancellationToken cancellationToken = default)
        {
            if (gameObject == null || string.IsNullOrWhiteSpace(newName))
            {
                return false;
            }

            var yamlBefore = EditorYaml.SerializeGameObject(gameObject);
            var clone = (GameObject)gameObject.Clone();
            clone.Name = newName.Trim();
            var yamlAfter = EditorYaml.SerializeGameObject(clone);

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new UpdateGameObjectCommand(gameObject, yamlBefore, yamlAfter, $"Rename {gameObject.Name}"),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(RenameGameObjectAsync))),
                cancellationToken);
            return result.Succeeded;
        }

        public async Task<SceneSaveResult> SaveCurrentWorldAsync(
            bool confirmExisting = true,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var page = Application.Current?.Windows.FirstOrDefault()?.Page ?? Application.Current?.MainPage;
            if (page is null)
                return new SceneSaveResult(SceneSaveOutcome.Failed, Error: "The editor window is unavailable.");

            var assetsService = MauiProgram.GetService<AssetsService>();
            var worldAsset = CurrentWorldAsset ?? (IsCurrentWorldUntitled ? null : ResolveCurrentWorldAsset());
            if (worldAsset is not null && assetsService.CanModifyAsset(worldAsset))
            {
                if (confirmExisting)
                {
                    var confirmed = await page.DisplayAlert(
                        "Save scene",
                        $"Save the current scene to {worldAsset.Asset.FullName}?",
                        "Save",
                        "Cancel");
                    if (!confirmed)
                        return new SceneSaveResult(SceneSaveOutcome.Cancelled);
                }

                return await SaveExistingWorldAsync(
                    worldAsset,
                    assetsService,
                    cancellationToken);
            }

            return await SaveCurrentWorldAsAsync(page, assetsService, cancellationToken);
        }

        public async Task<bool> CreateNewWorldAsync(CancellationToken cancellationToken = default)
        {
            var commandHistory = MauiProgram.GetService<ICommandHistoryService>();
            await commandHistory.BeginDocumentChangeAsync(cancellationToken);
            try
            {
                if (!await engineService.CreateWorldAsync(
                        cancellationToken))
                    return false;

                commandHistory.ResetForDocumentChange();
                CurrentWorldAsset = null;
                IsCurrentWorldUntitled = true;
                MauiProgram.GetService<SelectionService>().ResetForDocumentChange();
                return true;
            }
            finally
            {
                commandHistory.CompleteDocumentChange();
            }
        }

        public async Task<bool> LoadWorldAsync(WorldFile worldFile, CancellationToken cancellationToken = default)
        {
            if (worldFile?.FileId is null || worldFile.FileId.IsEmpty())
            {
                return false;
            }

            var commandHistory = MauiProgram.GetService<ICommandHistoryService>();
            await commandHistory.BeginDocumentChangeAsync(cancellationToken);
            try
            {
                if (!await engineService.LoadWorldAsync(
                        worldFile.FileId,
                        cancellationToken))
                    return false;

                commandHistory.ResetForDocumentChange();
                CurrentWorldAsset = worldFile;
                IsCurrentWorldUntitled = false;
                MauiProgram.GetService<SelectionService>().ResetForDocumentChange();
                return true;
            }
            finally
            {
                commandHistory.CompleteDocumentChange();
            }
        }

        async Task<SceneSaveResult> SaveExistingWorldAsync(
            WorldFile worldAsset,
            AssetsService assetsService,
            CancellationToken cancellationToken)
        {
            try
            {
                var serializedWorld =
                    await engineService.SerializeCurrentWorldAsync(
                        cancellationToken);
                if (string.IsNullOrWhiteSpace(serializedWorld))
                    return new SceneSaveResult(SceneSaveOutcome.Failed, Error: "The native world could not be serialized.");

                if (!await assetsService.SaveExistingAssetAsync(
                        worldAsset.FileId,
                        () => File.WriteAllTextAsync(
                            worldAsset.Asset.FullName,
                            serializedWorld,
                            cancellationToken),
                        cancellationToken))
                {
                    return new SceneSaveResult(
                        SceneSaveOutcome.Failed,
                        worldAsset.Asset.FullName,
                        "The scene was saved, but the native registry rejected its targeted update.");
                }
                worldAsset.IsDirty = false;
                assetsService.Refresh();
                CurrentWorldAsset = assetsService.Assets.TryGetValue(worldAsset.FileId, out var refreshed)
                    ? refreshed as WorldFile ?? worldAsset
                    : worldAsset;
                IsCurrentWorldUntitled = false;
                return new SceneSaveResult(SceneSaveOutcome.Saved, worldAsset.Asset.FullName);
            }
            catch (Exception exception)
            {
                return new SceneSaveResult(SceneSaveOutcome.Failed, worldAsset.Asset?.FullName, exception.Message);
            }
        }

        async Task<SceneSaveResult> SaveCurrentWorldAsAsync(
            Page page,
            AssetsService assetsService,
            CancellationToken cancellationToken)
        {
            var activeRoot = assetsService.CurrentProjectRootPath;
            var suggestedName = string.Equals(Current?.Name, "New Scene", StringComparison.OrdinalIgnoreCase)
                ? "NewScene.world"
                : $"{Current?.Name ?? "NewScene"}.world";
            var requestedPath = await page.DisplayPromptAsync(
                "Save new scene",
                $"Create the scene inside the active Content root:{Environment.NewLine}{activeRoot}",
                "Save",
                "Cancel",
                initialValue: suggestedName);
            cancellationToken.ThrowIfCancellationRequested();
            if (string.IsNullOrWhiteSpace(requestedPath))
                return new SceneSaveResult(SceneSaveOutcome.Cancelled);

            if (!SceneDocumentContract.TryResolveSaveTarget(activeRoot, requestedPath, out var target, out var error) || target is null)
            {
                await page.DisplayAlert("Save scene", error, "OK");
                return new SceneSaveResult(SceneSaveOutcome.Failed, Error: error);
            }

            var existingWorld = assetsService.Files
                .OfType<WorldFile>()
                .FirstOrDefault(x => x.Asset is not null &&
                    ProjectContentPathPolicy.IsSamePath(x.Asset.FullName, target.ScenePath));
            var sourceExists = File.Exists(target.ScenePath);
            var metadataExists = File.Exists(target.AssetInfoPath);
            if (sourceExists || metadataExists)
            {
                if (existingWorld is null && metadataExists)
                {
                    const string conflictError = "The target metadata already exists but is not a registered world asset.";
                    await page.DisplayAlert("Save scene", conflictError, "OK");
                    return new SceneSaveResult(SceneSaveOutcome.Failed, target.ScenePath, conflictError);
                }

                var overwrite = await page.DisplayAlert(
                    "Overwrite scene",
                    $"{target.Filename} already exists. Overwrite it?",
                    "Overwrite",
                    "Cancel");
                if (!overwrite)
                    return new SceneSaveResult(SceneSaveOutcome.Cancelled);
            }

            try
            {
                var serializedWorld =
                    await engineService.SerializeCurrentWorldAsync(
                        cancellationToken);
                if (string.IsNullOrWhiteSpace(serializedWorld))
                    return new SceneSaveResult(SceneSaveOutcome.Failed, Error: "The native world could not be serialized.");

                var directory = Path.GetDirectoryName(target.ScenePath)
                    ?? throw new InvalidOperationException("The scene directory is invalid.");
                Directory.CreateDirectory(directory);
                await File.WriteAllTextAsync(
                    target.ScenePath,
                    serializedWorld,
                    cancellationToken);

                var fileId = existingWorld?.FileId ?? new FileId($"{{{Guid.NewGuid().ToString().ToUpperInvariant()}}}");
                if (!metadataExists)
                {
                    await File.WriteAllTextAsync(
                        target.AssetInfoPath,
                        SceneDocumentContract.BuildAssetInfo(
                            fileId.Value,
                            target.Filename),
                        cancellationToken);
                }

                if (!await engineService.RequestAssetReloadAsync(
                        cancellationToken))
                {
                    return new SceneSaveResult(
                        SceneSaveOutcome.Failed,
                        target.ScenePath,
                        "The scene was saved, but the native registry rejected the reload.");
                }
                assetsService.Refresh();
                if (!assetsService.Assets.TryGetValue(fileId, out var asset) || asset is not WorldFile savedWorld)
                {
                    return new SceneSaveResult(
                        SceneSaveOutcome.Failed,
                        target.ScenePath,
                        "The saved scene was not discovered in the active Content root.");
                }

                CurrentWorldAsset = savedWorld;
                CurrentWorldAsset.IsDirty = false;
                IsCurrentWorldUntitled = false;
                return new SceneSaveResult(SceneSaveOutcome.Saved, target.ScenePath);
            }
            catch (Exception exception)
            {
                return new SceneSaveResult(SceneSaveOutcome.Failed, target.ScenePath, exception.Message);
            }
        }

        WorldFile ResolveCurrentWorldAsset()
        {
            var assets = MauiProgram.GetService<AssetsService>();
            var resolvedWorldAssets = assets.Assets.Values.OfType<WorldFile>();
            var byName = resolvedWorldAssets
                .FirstOrDefault(x => string.Equals(Path.GetFileNameWithoutExtension(x.Asset?.Name), Current?.Name, StringComparison.OrdinalIgnoreCase));

            CurrentWorldAsset = byName ?? resolvedWorldAssets
                .FirstOrDefault(x => string.Equals(x.Asset?.Name, "Editor.world", StringComparison.OrdinalIgnoreCase));

            return CurrentWorldAsset;
        }

        public async Task<bool> ReparentAsync(
            GameObject child,
            GameObject newParent,
            bool keepWorldTransform = true,
            CancellationToken cancellationToken = default)
        {
            if (child == null || newParent == null || ReferenceEquals(child, newParent) || IsDescendantOf(newParent, child))
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new ReparentGameObjectCommand(child, newParent, keepWorldTransform),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.DragDrop,
                        nameof(ReparentAsync))),
                cancellationToken);
            return result.Succeeded;
        }

        public async Task<bool> ReparentToRootAsync(
            GameObject child,
            bool keepWorldTransform = true,
            CancellationToken cancellationToken = default)
        {
            if (child == null ||
                ResolveParentInstanceId(child) is null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = await dispatcher.DispatchAsync(
                new ReparentGameObjectCommand(child, null, keepWorldTransform),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.DragDrop,
                        nameof(ReparentToRootAsync))),
                cancellationToken);
            return result.Succeeded;
        }

        public bool ApplyReparentLocal(InstanceId childId, InstanceId newParentId, bool keepWorldTransform = true)
        {
            if (!TryGetGameObject(childId, out var child))
            {
                return false;
            }

            GameObject newParent = null;
            if (newParentId is not null && !newParentId.IsEmpty())
            {
                if (!TryGetGameObject(newParentId, out newParent) || ReferenceEquals(child, newParent) || IsDescendantOf(newParent, child))
                {
                    return false;
                }
            }

            var worldPosition = keepWorldTransform ? GetWorldPosition(child) : Vector3.Zero;
            var worldRotation = keepWorldTransform ? GetWorldRotation(child) : Quaternion.Identity;

            if (newParent is not null && child.PrefabIndex != newParent.PrefabIndex)
            {
                MoveSubHierarchyToPrefab(child, Current.Prefabs[newParent.PrefabIndex]);
            }

            var prefab = Current.Prefabs[child.PrefabIndex];
            child.ParentIndex = newParent is null
                ? uint.MaxValue
                : (uint)prefab.GameObjects.IndexOf(newParent);

            if (keepWorldTransform)
            {
                ApplyLocalTransformFromWorld(child, newParent, worldPosition, worldRotation);
            }

            PublishCurrentWorld();
            return true;
        }

        public bool ApplyGameObjectYamlLocal(InstanceId instanceId, string yaml)
        {
            if (instanceId is null || instanceId.IsEmpty() || string.IsNullOrWhiteSpace(yaml))
            {
                return false;
            }

            if (!TryGetGameObject(instanceId, out var current) || current.PrefabIndex < 0 || current.PrefabIndex >= Current.Prefabs.Count)
            {
                return false;
            }

            var prefab = Current.Prefabs[current.PrefabIndex];
            var objectIndex = prefab.GameObjects.IndexOf(current);
            if (objectIndex < 0)
            {
                return false;
            }

            var updated = DeserializeGameObject(yaml);
            if (updated is null)
            {
                return false;
            }

            updated.Initialize();
            updated.PrefabIndex = current.PrefabIndex;

            prefab.GameObjects[objectIndex] = updated;
            gameObjectsDict.Remove(current.InstanceId);
            gameObjectsDict[updated.InstanceId] = updated;

            foreach (var componentIndex in current.ComponentIndices ?? [])
            {
                if (componentIndex >= 0 && componentIndex < prefab.Components.Count)
                {
                    componentOwnersDict.Remove(prefab.Components[componentIndex].InstanceId);
                }
            }

            foreach (var componentIndex in updated.ComponentIndices ?? [])
            {
                if (componentIndex >= 0 && componentIndex < prefab.Components.Count)
                {
                    var component = prefab.Components[componentIndex];
                    componentOwnersDict[component.InstanceId] = updated;
                    component.DisplayName = $"{updated.DisplayName} ({component.Typename.Name})";
                }
            }

            PublishCurrentWorld();
            return true;
        }

        public bool ApplyComponentYamlLocal(InstanceId instanceId, string yaml)
        {
            if (instanceId is null || instanceId.IsEmpty() || string.IsNullOrWhiteSpace(yaml))
            {
                return false;
            }

            if (!TryGetComponent(instanceId, out var current) || !componentOwnersDict.TryGetValue(instanceId, out var owner))
            {
                return false;
            }

            if (owner.PrefabIndex < 0 || owner.PrefabIndex >= Current.Prefabs.Count)
            {
                return false;
            }

            var prefab = Current.Prefabs[owner.PrefabIndex];
            var componentIndex = prefab.Components.IndexOf(current);
            if (componentIndex < 0)
            {
                return false;
            }

            var updated = DeserializeComponent(yaml);
            if (updated is null)
            {
                return false;
            }

            updated.Initialize();
            updated.DisplayName = $"{owner.DisplayName} ({updated.Typename.Name})";

            prefab.Components[componentIndex] = updated;
            componentsDict.Remove(current.InstanceId);
            componentsDict[updated.InstanceId] = updated;
            componentOwnersDict.Remove(current.InstanceId);
            componentOwnersDict[updated.InstanceId] = owner;

            PublishCurrentWorld();
            return true;
        }

        public IEnumerable<GameObject> EnumerateSubHierarchy(GameObject root)
        {
            var prefab = Current.Prefabs[root.PrefabIndex];
            var rootIndex = prefab.GameObjects.IndexOf(root);
            if (rootIndex < 0)
            {
                yield break;
            }

            var childrenByParent = new List<int>[prefab.GameObjects.Count];
            for (int i = 0; i < prefab.GameObjects.Count; i++)
            {
                var parentIndex = prefab.GameObjects[i].ParentIndex;
                if (parentIndex >= (uint)prefab.GameObjects.Count)
                    continue;

                childrenByParent[(int)parentIndex] ??= [];
                childrenByParent[(int)parentIndex].Add(i);
            }

            var pending = new Stack<int>();
            var visited = new HashSet<int>();
            pending.Push(rootIndex);
            while (pending.TryPop(out var index))
            {
                if (!visited.Add(index))
                    continue;

                yield return prefab.GameObjects[index];

                var children = childrenByParent[index];
                if (children is null)
                    continue;

                for (int i = children.Count - 1; i >= 0; i--)
                    pending.Push(children[i]);
            }
        }

        public Prefab CreatePrefabFromSubHierarchy(GameObject root, out List<InstanceId> externalSceneRefs)
        {
            var sourcePrefab = Current.Prefabs[root.PrefabIndex];
            var sourceObjectIndicesByObject = sourcePrefab.GameObjects
                .Select((gameObject, index) => new { gameObject, index })
                .ToDictionary(entry => entry.gameObject, entry => entry.index);
            var sourceObjects = EnumerateSubHierarchy(root).ToList();
            var sourceObjectIndices = sourceObjects
                .Select(gameObject => sourceObjectIndicesByObject[gameObject])
                .ToHashSet();
            var sourceComponentIndices = sourceObjects
                .SelectMany(go => go.ComponentIndices)
                .Distinct()
                .OrderBy(index => index)
                .ToList();

            var componentIndexMap = sourceComponentIndices
                .Select((sourceIndex, index) => new { sourceIndex, index })
                .ToDictionary(entry => entry.sourceIndex, entry => entry.index);

            var sourceObjectIndexMap = sourceObjects
                .Select((gameObject, index) => new
                {
                    sourceIndex = sourceObjectIndicesByObject[gameObject],
                    index
                })
                .ToDictionary(entry => entry.sourceIndex, entry => entry.index);

            var prefab = new Prefab();
            foreach (var componentIndex in sourceComponentIndices)
            {
                prefab.Components.Add(CloneComponent(
                    sourcePrefab.Components[componentIndex]));
            }

            foreach (var go in sourceObjects)
            {
                var clone = CloneGameObject(go);
                clone.ComponentIndices = go.ComponentIndices.Select(index => componentIndexMap[index]).ToList();
                clone.ParentIndex = go == root || !sourceObjectIndices.Contains((int)go.ParentIndex)
                    ? uint.MaxValue
                    : (uint)sourceObjectIndexMap[(int)go.ParentIndex];
                prefab.GameObjects.Add(clone);
            }

            externalSceneRefs = FindExternalSceneRefs(prefab);
            return prefab;
        }

        public void PopulateWorld(string yaml) => TryPopulateWorld(yaml, WorkspaceEpoch);

        public bool TryPopulateWorld(string yaml, long expectedWorkspaceEpoch)
        {
            if (expectedWorkspaceEpoch != WorkspaceEpoch)
                return false;

            using var perfScope = EditorPerf.Scope("WorldService.PopulateWorld");

            var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new WorldYamlConverter())
            .Build();

            var world = deserializer.Deserialize<World>(yaml);
            if (world == null)
                return false;

            if (expectedWorkspaceEpoch != WorkspaceEpoch)
                return false;

            GameObjects.Clear();
            Current.Prefabs.Clear();

            Current.Name = world.Name;

            var worldKey = GetWorldKey(world);
            if (!worldCaches.TryGetValue(worldKey, out var cache))
            {
                cache = new WorldCache();
                worldCaches[worldKey] = cache;
            }

            // Rebuild only this world's lookup caches.
            cache.Components.Clear();
            cache.GameObjects.Clear();
            cache.ComponentOwners.Clear();
            currentCache = cache;

            int prefabIndex = 0;
            foreach (var prefab in world.Prefabs)
            {
                prefab.GameObjects ??= [];
                prefab.Components ??= [];

                var newPrefab = (Prefab)prefab.Clone();
                newPrefab.GameObjects.Clear();
                newPrefab.Components.Clear();

                foreach (var component in prefab.Components)
                {
                    component.Initialize();
                    componentsDict[component.InstanceId] = component;
                    newPrefab.Components.Add(component);
                }

                foreach (var go in prefab.GameObjects)
                {
                    var gameObject = go;
                    gameObject.Initialize();
                    gameObjectsDict[go.InstanceId] = gameObject;

                    gameObject.PrefabIndex = prefabIndex;
                    foreach (var i in go.ComponentIndices ?? [])
                    {
                        var component = prefab.Components[i];
                        componentOwnersDict[component.InstanceId] = gameObject;
                        component.DisplayName = $"{go.DisplayName} ({component.Typename.Name})";
                    }

                    newPrefab.GameObjects.Add(gameObject);
                }

                Current.Prefabs.Add(newPrefab);
                GameObjects.Add([.. newPrefab.GameObjects]);

                prefabIndex++;
            }

            PublishCurrentWorld();
            return true;
        }

        void PublishCurrentWorld()
        {
            RefreshSelection();
            OnUpdateWorldAction?.Invoke(Current);
        }

        void RefreshSelection()
        {
            var selectionService = MauiProgram.GetService<SelectionService>();
            var selectedItem = selectionService.SelectedItem;

            if (selectedItem is GameObject selectedGameObject)
            {
                if (TryGetGameObject(selectedGameObject.InstanceId, out var refreshedGameObject))
                {
                    selectionService.SelectObject(refreshedGameObject, force: true);
                }
                else
                {
                    selectionService.ClearSelection();
                }
            }
            else if (selectedItem is Component selectedComponent)
            {
                if (TryGetComponent(selectedComponent.InstanceId, out var refreshedComponent))
                {
                    selectionService.SelectObject(refreshedComponent, force: true);
                }
                else
                {
                    selectionService.ClearSelection();
                }
            }
        }

        void MoveSubHierarchyToPrefab(GameObject root, Prefab targetPrefab)
        {
            var sourcePrefab = Current.Prefabs[root.PrefabIndex];
            var oldSourceObjects = sourcePrefab.GameObjects.ToList();
            var movingObjects = EnumerateSubHierarchy(root).ToList();
            var movingObjectsSet = movingObjects.ToHashSet();
            var sourceObjectIndexMap = movingObjects
                .Select(go => new { GameObject = go, Index = oldSourceObjects.IndexOf(go) })
                .ToDictionary(x => x.GameObject, x => x.Index);
            var movingComponents = movingObjects.SelectMany(go => go.ComponentIndices).Distinct().OrderBy(index => index).ToList();
            var movingComponentSet = movingComponents.ToHashSet();

            var componentIndexMap = new Dictionary<int, int>();
            foreach (var componentIndex in movingComponents)
            {
                componentIndexMap[componentIndex] = targetPrefab.Components.Count;
                targetPrefab.Components.Add(sourcePrefab.Components[componentIndex]);
            }

            var sourceComponentIndexMap = new Dictionary<int, int>();
            var remainingComponents = new ObservableList<Component>();
            for (int i = 0; i < sourcePrefab.Components.Count; i++)
            {
                if (movingComponentSet.Contains(i))
                {
                    continue;
                }

                sourceComponentIndexMap[i] = remainingComponents.Count;
                remainingComponents.Add(sourcePrefab.Components[i]);
            }

            var targetGameObjectStartIndex = targetPrefab.GameObjects.Count;
            var oldObjectIndexToTargetIndex = new Dictionary<int, int>();
            for (int i = 0; i < movingObjects.Count; i++)
            {
                oldObjectIndexToTargetIndex[sourceObjectIndexMap[movingObjects[i]]] = targetGameObjectStartIndex + i;
            }

            var remainingObjects = oldSourceObjects.Where(go => !movingObjectsSet.Contains(go)).ToList();
            var remainingObjectIndexMap = remainingObjects
                .Select((go, index) => new { OldIndex = oldSourceObjects.IndexOf(go), NewIndex = index })
                .ToDictionary(x => x.OldIndex, x => x.NewIndex);

            foreach (var go in remainingObjects)
            {
                go.ComponentIndices = go.ComponentIndices.Select(index => sourceComponentIndexMap[index]).ToList();

                if (go.ParentIndex != uint.MaxValue)
                {
                    go.ParentIndex = remainingObjectIndexMap.TryGetValue((int)go.ParentIndex, out var parentIndex)
                        ? (uint)parentIndex
                        : uint.MaxValue;
                }
            }

            sourcePrefab.GameObjects = new ObservableList<GameObject>(remainingObjects);
            sourcePrefab.Components = remainingComponents;

            foreach (var go in movingObjects)
            {
                targetPrefab.GameObjects.Add(go);
                go.PrefabIndex = Current.Prefabs.IndexOf(targetPrefab);
                go.ComponentIndices = go.ComponentIndices.Select(index => componentIndexMap[index]).ToList();

                if (go == root || !oldObjectIndexToTargetIndex.ContainsKey((int)go.ParentIndex))
                {
                    go.ParentIndex = uint.MaxValue;
                }
                else
                {
                    go.ParentIndex = (uint)oldObjectIndexToTargetIndex[(int)go.ParentIndex];
                }
            }
        }

        bool IsDescendantOf(GameObject candidate, GameObject parent)
        {
            var prefab = Current.Prefabs[candidate.PrefabIndex];
            var current = candidate;
            while (current.ParentIndex != uint.MaxValue)
            {
                current = prefab.GameObjects[(int)current.ParentIndex];
                if (ReferenceEquals(current, parent))
                {
                    return true;
                }
            }

            return false;
        }

        Vector3 GetWorldPosition(GameObject gameObject)
        {
            var prefab = Current.Prefabs[gameObject.PrefabIndex];
            var localPosition = ToVector3(gameObject.Position);
            var current = gameObject;
            var result = localPosition;
            while (current.ParentIndex != uint.MaxValue)
            {
                var parent = prefab.GameObjects[(int)current.ParentIndex];
                result = ToVector3(parent.Position) + Vector3.Transform(result, ToQuaternion(parent.Rotation));
                current = parent;
            }

            return result;
        }

        Quaternion GetWorldRotation(GameObject gameObject)
        {
            var prefab = Current.Prefabs[gameObject.PrefabIndex];
            var result = ToQuaternion(gameObject.Rotation);
            var current = gameObject;
            while (current.ParentIndex != uint.MaxValue)
            {
                var parent = prefab.GameObjects[(int)current.ParentIndex];
                result = Quaternion.Normalize(ToQuaternion(parent.Rotation) * result);
                current = parent;
            }

            return result;
        }

        static Vector3 ToVector3(Vec4 position) => new(position.X, position.Y, position.Z);

        static Quaternion ToQuaternion(Rotation rotation)
        {
            var quat = rotation?.Quat ?? Quat.FromYawPitchRoll(rotation?.Yaw ?? 0.0f, rotation?.Pitch ?? 0.0f, rotation?.Roll ?? 0.0f);
            var result = new Quaternion(quat.X, quat.Y, quat.Z, quat.W);
            return result.LengthSquared() > 0.0f ? Quaternion.Normalize(result) : Quaternion.Identity;
        }

        static Quat ToSailorQuat(Quaternion rotation)
        {
            rotation = rotation.LengthSquared() > 0.0f ? Quaternion.Normalize(rotation) : Quaternion.Identity;
            return new Quat { X = rotation.X, Y = rotation.Y, Z = rotation.Z, W = rotation.W };
        }

        void ApplyLocalTransformFromWorld(GameObject child, GameObject parent, Vector3 worldPosition, Quaternion worldRotation)
        {
            var localPosition = worldPosition;
            var localRotation = worldRotation;

            if (parent is not null)
            {
                var parentWorldRotation = GetWorldRotation(parent);
                localPosition = Vector3.Transform(worldPosition - GetWorldPosition(parent), Quaternion.Inverse(parentWorldRotation));
                localRotation = Quaternion.Normalize(Quaternion.Inverse(parentWorldRotation) * worldRotation);
            }

            child.Position ??= new Vec4();
            child.Position.X = localPosition.X;
            child.Position.Y = localPosition.Y;
            child.Position.Z = localPosition.Z;

            child.Rotation ??= new Rotation();
            child.Rotation.Quat = ToSailorQuat(localRotation);
        }

        static GameObject CloneGameObject(GameObject gameObject)
        {
            return new GameObject
            {
                Name = gameObject.Name,
                InstanceId = new InstanceId(gameObject.InstanceId.Value),
                ParentIndex = gameObject.ParentIndex,
                Position = new Vec4(gameObject.Position),
                Rotation = new Rotation(gameObject.Rotation),
                Scale = new Vec4(gameObject.Scale),
                ComponentIndices = [.. gameObject.ComponentIndices]
            };
        }

        static Component CloneComponent(Component component)
        {
            var clone = new Component
            {
                DisplayName = component.DisplayName,
                Typename = component.Typename
            };

            foreach (var property in component.OverrideProperties)
            {
                clone.OverrideProperties[property.Key] =
                    CloneComponentProperty(property.Value);
            }

            foreach (var property in component.PreservedReadOnlyProperties)
            {
                clone.PreservedReadOnlyProperties[property.Key] =
                    property.Value;
            }

            return clone;
        }

        static ObservableObject CloneComponentProperty(
            ObservableObject property)
        {
            if (property is Observable<FileId> fileId)
            {
                return new Observable<FileId>(
                    (FileId)fileId.Value.Clone());
            }

            if (property is Observable<InstanceId> instanceId)
            {
                return new Observable<InstanceId>(
                    (InstanceId)instanceId.Value.Clone());
            }

            if (property is ObservableFileIdList fileIds)
            {
                return new ObservableFileIdList(fileIds.Values.Select(
                    fileId => (FileId)fileId.Value.Clone()));
            }

            if (property is ObjectPtr objectPtr)
            {
                return new ObjectPtr
                {
                    FileId = (FileId)objectPtr.FileId.Clone(),
                    InstanceId = (InstanceId)objectPtr.InstanceId.Clone()
                };
            }

            if (property is ICloneable cloneable &&
                cloneable.Clone() is ObservableObject clone)
            {
                return clone;
            }

            throw new InvalidOperationException(
                $"Cannot clone component property '{property.GetType().Name}'.");
        }

        static GameObject DeserializeGameObject(string yaml)
        {
            return SerializationUtils.CreateDeserializerBuilder()
                .Build()
                .Deserialize<GameObject>(yaml);
        }

        static Component DeserializeComponent(string yaml)
        {
            return SerializationUtils.CreateDeserializerBuilder()
                .WithTypeConverter(new ComponentYamlConverter())
                .Build()
                .Deserialize<Component>(yaml);
        }

        static List<InstanceId> FindExternalSceneRefs(Prefab prefab)
        {
            var internalIds = prefab.GameObjects.Select(go => go.InstanceId)
                .Concat(prefab.Components.Select(component => component.InstanceId))
                .ToHashSet();

            var result = new List<InstanceId>();
            foreach (var component in prefab.Components)
            {
                foreach (var value in component.OverrideProperties.Values)
                {
                    CollectInstanceRefs(value, internalIds, result);
                }
            }

            return result;
        }

        static void CollectInstanceRefs(object value, HashSet<InstanceId> internalIds, List<InstanceId> externalRefs)
        {
            switch (value)
            {
                case Observable<InstanceId> observableId:
                    AddIfExternal(observableId.Value, internalIds, externalRefs);
                    break;
                case ObjectPtr objectPtr:
                    AddIfExternal(objectPtr.InstanceId, internalIds, externalRefs);
                    break;
            }
        }

        static void AddIfExternal(InstanceId id, HashSet<InstanceId> internalIds, List<InstanceId> externalRefs)
        {
            if (id == null || id.IsEmpty() || internalIds.Contains(id) || externalRefs.Contains(id))
            {
                return;
            }

            externalRefs.Add(id);
        }

        public string SerializeCurrentWorld()
        {
            string yamlWorld = string.Empty;

            using (var writer = new StringWriter())
            {
                var serializer = SerializationUtils.CreateSerializerBuilder()
                .WithTypeConverter(new WorldYamlConverter())
                .Build();

                var yaml = serializer.Serialize(Current);
                writer.Write(yaml);

                yamlWorld = writer.ToString();
            }

            return yamlWorld;
        }

        readonly Dictionary<string, WorldCache> worldCaches = new();
        WorldCache currentCache = new();

        Dictionary<InstanceId, Component> componentsDict => currentCache.Components;
        Dictionary<InstanceId, GameObject> gameObjectsDict => currentCache.GameObjects;
        Dictionary<InstanceId, GameObject> componentOwnersDict => currentCache.ComponentOwners;
    }
}
