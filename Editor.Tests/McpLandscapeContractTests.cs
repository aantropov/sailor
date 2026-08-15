namespace Editor.Tests;

public sealed class McpLandscapeContractTests
{
    [Fact]
    public void Mcp_ExposesTypedLandscapeAuthoringAndRegenerationTools()
    {
        var tools = ReadRepositoryFile("Editor", "Mcp", "McpEditorTools.cs");
        var contracts = ReadRepositoryFile("Editor", "Mcp", "McpLandscapeContracts.cs");

        Assert.Contains("sailor_landscape_apply", tools, StringComparison.Ordinal);
        Assert.Contains("sailor_landscape_regenerate", tools, StringComparison.Ordinal);
        Assert.Contains("McpLandscapeVegetationProfile", contracts, StringComparison.Ordinal);
        Assert.Contains("McpLandscapeSculptStamp", contracts, StringComparison.Ordinal);
        Assert.Contains("McpLandscapePaintStamp", contracts, StringComparison.Ordinal);
        Assert.Contains("HeightmapTextureFileId", contracts, StringComparison.Ordinal);
        Assert.Contains("MaterialMaskFileIds", contracts, StringComparison.Ordinal);
    }

    [Fact]
    public void Mapping_PacksAllVegetationControlsAndAuthoringStamps()
    {
        var source = ReadRepositoryFile("Editor", "Mcp", "McpLandscapeOperations.cs");

        Assert.Contains("vegetationModels", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMaterials", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMeshIndex", source, StringComparison.Ordinal);
        Assert.Contains("vegetationInstancesPerChunk", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMinScale", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMaxScale", source, StringComparison.Ordinal);
        Assert.Contains("vegetationGroundOffset", source, StringComparison.Ordinal);
        Assert.Contains("vegetationShadowMode", source, StringComparison.Ordinal);
        Assert.Contains("vegetationShadowDistance", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMinLod", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMaxLod", source, StringComparison.Ordinal);
        Assert.Contains("vegetationLod1ScreenCoverage", source, StringComparison.Ordinal);
        Assert.Contains("vegetationLod2ScreenCoverage", source, StringComparison.Ordinal);
        Assert.Contains("vegetationCullDistance", source, StringComparison.Ordinal);
        Assert.Contains("vegetationColliderRadius", source, StringComparison.Ordinal);
        Assert.Contains("vegetationColliderHeight", source, StringComparison.Ordinal);
        Assert.Contains("vegetationColliderOffsetY", source, StringComparison.Ordinal);
        Assert.Contains("heightmapTexture", source, StringComparison.Ordinal);
        Assert.Contains("materialMasks", source, StringComparison.Ordinal);
        Assert.Contains("sculptStamps", source, StringComparison.Ordinal);
        Assert.Contains("paintStamps", source, StringComparison.Ordinal);
        Assert.Contains("None, NearOnly, or All", source, StringComparison.Ordinal);
        Assert.Contains("an empty materialFileId uses the GLB materials", source, StringComparison.Ordinal);
        Assert.DoesNotContain(
            "string.IsNullOrWhiteSpace(profile.MaterialFileId)",
            source,
            StringComparison.Ordinal);
        var tools = ReadRepositoryFile("Editor", "Mcp", "McpEditorTools.cs");
        Assert.Contains(
            "leave it empty to use the GLB materials",
            tools,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Inspector_HidesBrushDataButKeepsGenerationActions()
    {
        var source = ReadRepositoryFile(
            "Editor", "Views", "InspectorView", "ComponentTemplate.Landscape.cs");
        var template = ReadRepositoryFile(
            "Editor", "Views", "InspectorView", "ComponentTemplate.cs");

        Assert.Contains("Text = \"Generate\"", source, StringComparison.Ordinal);
        Assert.Contains("Text = \"Regenerate\"", source, StringComparison.Ordinal);
        Assert.Contains("Text = \"Flatten\"", source, StringComparison.Ordinal);
        Assert.Contains("Text = \"Vegetation\"", source, StringComparison.Ordinal);
        Assert.Contains("Text = \"+ Add\"", source, StringComparison.Ordinal);
        Assert.Contains("typeof(ModelFile)", source, StringComparison.Ordinal);
        Assert.Contains("Material override", source, StringComparison.Ordinal);
        Assert.Contains("typeof(MaterialFile)", source, StringComparison.Ordinal);
        Assert.Contains("EnsureLandscapeProfileLength(materials, models.Values.Count)", source, StringComparison.Ordinal);
        Assert.Contains("\"None\", \"Near only\", \"All\"", source, StringComparison.Ordinal);
        Assert.Contains("\"Mesh index\"", source, StringComparison.Ordinal);
        Assert.Contains("\"Cull distance\"", source, StringComparison.Ordinal);
        Assert.Contains("\"Collider radius\"", source, StringComparison.Ordinal);
        Assert.DoesNotContain("GetOrCreateLandscapeFloatList", source, StringComparison.Ordinal);
        var component = ReadRepositoryFile("Editor", "ViewModels", "Component.cs");
        Assert.DoesNotContain("AddMissingLandscapeVegetationProperties", component, StringComparison.Ordinal);
        Assert.Contains("Import landscape heightmap", source, StringComparison.Ordinal);
        Assert.Contains("Import Material Masks", source, StringComparison.Ordinal);
        Assert.Contains("RebuildLandscapeAsync(component, advanceSeed: true)", source, StringComparison.Ordinal);
        Assert.Contains("await component.CommitInspectorChangesAsync()", source, StringComparison.Ordinal);
        Assert.Contains("await component.ApplyInspectorBatchAsync", source, StringComparison.Ordinal);
        Assert.True(
            template.IndexOf("foreach (var property in EnumerateInspectorProperties", StringComparison.Ordinal) <
            template.IndexOf("AddLandscapeVegetationEditor(props, component);", StringComparison.Ordinal),
            "Vegetation settings must be rendered after the regular landscape properties.");
        Assert.Contains("ApplyInspectorBatchAsync", component, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay(125)", source, StringComparison.Ordinal);
        Assert.DoesNotContain("Text = \"Raise\"", source, StringComparison.Ordinal);
        Assert.DoesNotContain("Text = \"Lower\"", source, StringComparison.Ordinal);
        Assert.DoesNotContain("Text = \"Smooth\"", source, StringComparison.Ordinal);
        Assert.DoesNotContain("Text = \"Paint Layer\"", source, StringComparison.Ordinal);
    }

    [Fact]
    public void FileIdSerialization_AllowsMissingLandscapeImportAssets()
    {
        var source = ReadRepositoryFile("Editor", "Utility", "EngineTypes.cs");

        Assert.Contains("parser.Consume<Scalar>().Value ?? string.Empty", source, StringComparison.Ordinal);
        Assert.Contains("(value as FileId)?.Value ?? string.Empty", source, StringComparison.Ordinal);
    }

    [Fact]
    public void Runtime_UsesGlbMaterialsWhenVegetationOverrideIsEmpty()
    {
        var source = ReadRepositoryFile("Runtime", "ECS", "LandscapeECS.cpp");

        Assert.Contains("models.Num()", source, StringComparison.Ordinal);
        Assert.Contains("LoadDefaultMaterials(", source, StringComparison.Ordinal);
        Assert.Contains("profile.m_modelFileId, profile.m_modelMaterials", source, StringComparison.Ordinal);
        Assert.Contains("ResolveMaterialIndex(", source, StringComparison.Ordinal);
        Assert.Contains("vegetationMaterials[meshIndex]", source, StringComparison.Ordinal);
        Assert.DoesNotContain("!profile.m_modelFileId || !profile.m_materialFileId", source, StringComparison.Ordinal);
        Assert.DoesNotContain("m_buildRevision", source, StringComparison.Ordinal);
        Assert.DoesNotContain("revision %", source, StringComparison.Ordinal);
        var header = ReadRepositoryFile("Runtime", "ECS", "LandscapeECS.h");
        Assert.DoesNotContain("m_buildRevision", header, StringComparison.Ordinal);
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return File.ReadAllText(Path.Combine([current.FullName, .. relativePath]));
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
