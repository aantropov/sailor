using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.ViewModels;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.Core.Tokens;
using System.Numerics;

namespace SailorEditor.Services
{
    public partial class WorldService : ObservableObject
    {
        class WorldCache
        {
            public Dictionary<InstanceId, Component> Components { get; } = new();
            public Dictionary<InstanceId, GameObject> GameObjects { get; } = new();
        }

        public World Current { get; private set; } = new World();

        [ObservableProperty]
        ObservableList<ObservableList<GameObject>> gameObjects = new();

        public event Action<World> OnUpdateWorldAction = delegate { };

        public WorldService()
        {
            MauiProgram.GetService<EngineService>().OnUpdateCurrentWorldAction += PopulateWorld;
        }

        static string GetWorldKey(World world) => string.IsNullOrEmpty(world?.Name) ? "__default_world__" : world.Name;

        public Component GetComponent(InstanceId instanceId) => componentsDict[instanceId];
        public GameObject GetGameObject(InstanceId instanceId) => gameObjectsDict[instanceId];
        public bool TryGetComponent(InstanceId instanceId, out Component component) => componentsDict.TryGetValue(instanceId, out component);
        public bool TryGetGameObject(InstanceId instanceId, out GameObject gameObject) => gameObjectsDict.TryGetValue(instanceId, out gameObject);

        public List<Component> GetComponents(GameObject gameObject)
        {
            List<Component> res = new();
            foreach (var componentIndex in gameObject.ComponentIndices)
            {
                res.Add(Current.Prefabs[gameObject.PrefabIndex].Components[componentIndex]);
            }

            return res;
        }

        public Component FindComponent(GameObject gameObject, string componentTypeName)
        {
            return GetComponents(gameObject).FirstOrDefault(component => component.Typename.Name == componentTypeName);
        }

        public bool CreateGameObject(GameObject parent = null)
        {
            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new CreateGameObjectCommand(parent?.InstanceId),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(CreateGameObject))))
                .GetAwaiter()
                .GetResult();
            return result.Succeeded;
        }

        public bool RemoveGameObject(GameObject gameObject)
        {
            if (gameObject == null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new DestroyGameObjectCommand(gameObject),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(RemoveGameObject))))
                .GetAwaiter()
                .GetResult();
            if (result.Succeeded)
            {
                MauiProgram.GetService<SelectionService>().ClearSelection();
            }

            return result.Succeeded;
        }

        public bool ResetComponentToDefaults(Component component)
        {
            if (component == null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new ResetComponentToDefaultsCommand(component),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(ResetComponentToDefaults))))
                .GetAwaiter()
                .GetResult();
            return result.Succeeded;
        }

        public bool AddComponent(GameObject gameObject, string componentTypeName)
        {
            if (gameObject == null || string.IsNullOrWhiteSpace(componentTypeName))
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new AddComponentCommand(gameObject, componentTypeName),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(AddComponent))))
                .GetAwaiter()
                .GetResult();
            return result.Succeeded;
        }

        public bool RemoveComponent(Component component)
        {
            if (component == null)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new RemoveComponentCommand(component),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(RemoveComponent))))
                .GetAwaiter()
                .GetResult();
            if (result.Succeeded && ReferenceEquals(MauiProgram.GetService<SelectionService>().SelectedItem, component))
            {
                MauiProgram.GetService<SelectionService>().ClearSelection();
            }

            return result.Succeeded;
        }

        public bool RenameGameObject(GameObject gameObject, string newName)
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
            var result = dispatcher.DispatchAsync(
                new UpdateGameObjectCommand(gameObject, yamlBefore, yamlAfter, $"Rename {gameObject.Name}"),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(RenameGameObject))))
                .GetAwaiter()
                .GetResult();
            return result.Succeeded;
        }

        public bool Reparent(GameObject child, GameObject newParent, bool keepWorldTransform = true)
        {
            if (child == null || newParent == null || ReferenceEquals(child, newParent) || IsDescendantOf(newParent, child))
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new ReparentGameObjectCommand(child, newParent, keepWorldTransform),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(Reparent))))
                .GetAwaiter()
                .GetResult();
            return result.Succeeded;
        }

        public bool ReparentToRoot(GameObject child, bool keepWorldTransform = true)
        {
            if (child == null || child.ParentIndex == uint.MaxValue)
            {
                return false;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var result = dispatcher.DispatchAsync(
                new ReparentGameObjectCommand(child, null, keepWorldTransform),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(ReparentToRoot))))
                .GetAwaiter()
                .GetResult();
            return result.Succeeded;
        }

        public IEnumerable<GameObject> EnumerateSubHierarchy(GameObject root)
        {
            var prefab = Current.Prefabs[root.PrefabIndex];
            yield return root;

            var rootIndex = prefab.GameObjects.IndexOf(root);
            foreach (var child in prefab.GameObjects.Where(go => go.ParentIndex == rootIndex))
            {
                foreach (var descendant in EnumerateSubHierarchy(child))
                {
                    yield return descendant;
                }
            }
        }

        public Prefab CreatePrefabFromSubHierarchy(GameObject root, out List<InstanceId> externalSceneRefs)
        {
            var sourcePrefab = Current.Prefabs[root.PrefabIndex];
            var sourceObjects = EnumerateSubHierarchy(root).ToList();
            var sourceObjectIndices = sourceObjects.Select(go => sourcePrefab.GameObjects.IndexOf(go)).ToHashSet();
            var sourceComponents = sourceObjects
                .SelectMany(go => go.ComponentIndices)
                .Distinct()
                .OrderBy(index => index)
                .Select(index => sourcePrefab.Components[index])
                .ToList();

            var componentIndexMap = sourceComponents
                .Select((component, index) => new { component, index })
                .ToDictionary(x => sourcePrefab.Components.IndexOf(x.component), x => x.index);

            var sourceObjectIndexMap = sourceObjects
                .Select((go, index) => new { sourceIndex = sourcePrefab.GameObjects.IndexOf(go), index })
                .ToDictionary(x => x.sourceIndex, x => x.index);

            var prefab = new Prefab();
            foreach (var component in sourceComponents)
            {
                prefab.Components.Add(CloneComponent(component));
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

        public void PopulateWorld(string yaml)
        {
            var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new WorldYamlConverter())
            .Build();

            var world = deserializer.Deserialize<World>(yaml);
            if (world == null)
                return;

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
            currentCache = cache;

            int prefabIndex = 0;
            foreach (var prefab in world.Prefabs)
            {
                prefab.GameObjects ??= [];
                prefab.Components ??= [];

                var newPrefab = new Prefab();
                foreach (var go in prefab.GameObjects)
                {
                    var gameObject = go;
                    gameObject.Initialize();
                    gameObjectsDict[go.InstanceId] = gameObject;

                    gameObject.PrefabIndex = prefabIndex;
                    foreach (var i in go.ComponentIndices ?? [])
                    {
                        var component = prefab.Components[i];
                        component.Initialize();
                        componentsDict[component.InstanceId] = component;
                        component.DisplayName = $"{go.DisplayName} ({component.Typename.Name})";

                        newPrefab.Components.Add(component);
                    }

                    newPrefab.GameObjects.Add(gameObject);
                }

                Current.Prefabs.Add(newPrefab);
                GameObjects.Add([.. newPrefab.GameObjects]);

                prefabIndex++;
            }

            OnUpdateWorldAction?.Invoke(Current);
            RefreshSelection();
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
            var yaml = SerializationUtils.CreateSerializerBuilder()
                .WithTypeConverter(new ComponentYamlConverter())
                .Build()
                .Serialize(component);

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
    }
}
