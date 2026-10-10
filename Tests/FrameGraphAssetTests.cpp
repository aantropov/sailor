#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "FrameGraph/FrameGraphNode.h"
#include "Support/TempDirectory.h"
#include "Workspace/WorkspaceContext.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace Sailor
{
	class FrameGraphImporterTestAccess
	{
	public:
		static RHI::RHIFrameGraphPtr Build(const FrameGraphImporter& importer, const FrameGraphAssetPtr& asset)
		{
			auto instance = importer.BuildFrameGraph(FileId::CreateNewFileId(), asset);
			auto graph = instance->GetRHI();
			instance.DestroyObject(importer.m_allocator);
			return graph;
		}

		static glm::vec4 GetValue(const RHI::RHIFrameGraph& graph, StringHash name)
		{
			const glm::vec4* value = nullptr;
			if (!graph.m_values.Find(name, value))
			{
				throw std::runtime_error("Missing built frame-graph value: " + name.ToString());
			}
			return *value;
		}

		static size_t GetValueCount(const RHI::RHIFrameGraph& graph)
		{
			return graph.m_values.Num();
		}
	};
}

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void TestRuntimeNamesAndValues()
	{
		RHI::RHIFrameGraph graph;
		auto node = TRefPtr<Framegraph::RHINodeDefault>::Make();
		graph.GetGraph().Add(node);
		{
			std::string names = "prefix:BorrowedNode:suffix";
			node->SetTag(StringHash::Runtime(std::string_view(names).substr(7, 12)));
			std::string value = "prefix:kept text:suffix";
			node->SetString("label"_h, std::string_view(value).substr(7, 9));
			node->SetFloat("gain"_h, 0.75f);
		}
		Require(graph.GetGraphNode("BorrowedNode"_h) == node && node->GetString("label"_h) == "kept text" &&
			node->GetFloat("gain"_h) == 0.75f && node->GetTag().ToString() == "BorrowedNode",
			"frame-graph names and values must outlive their source views and match literal lookups");

		std::string_view borrowed;
		std::string owned;
		Require(node->TryGetString("label"_h, borrowed) && borrowed == "kept text" &&
			borrowed.data() == node->GetString("label"_h).data(),
			"borrowed lookup must return a view of the stored parameter without copying it");
		Require(node->TryGetString("label"_h, owned) && owned == borrowed,
			"owned and borrowed lookup must return the same parameter value");
		Require(!node->TryGetString("missing"_h, borrowed) && borrowed == "kept text" &&
			!node->TryGetString("missing"_h, owned) && owned == "kept text",
			"a missing parameter must leave both output forms unchanged");
		node->SetString("label"_h, "replacement");
		Require(owned == "kept text" && node->TryGetString("label"_h, borrowed) && borrowed == "replacement",
			"an owned snapshot must survive edits, while a fresh view must read the updated parameter");
		node->SetString("label"_h, "");
		Require(node->TryGetString("label"_h, borrowed) && borrowed.empty(),
			"an empty parameter is a successful lookup, not a missing one");
	}

	void TestGpuTimingNames()
	{
		auto node = TRefPtr<Framegraph::RHINodeDefault>::Make();
		StringHash original;
		{
			std::string source = "prefix:Lighting:suffix";
			node->SetTag(StringHash::Runtime(std::string_view(source).substr(7, 8)));
			node->SetString("Tag"_h, "Opaque");
			source = "prefix:Shaders/Lighting.shader:suffix";
			node->SetString("shader"_h, std::string_view(source).substr(7, 23));
			original = node->GetGpuTimingName(3);
			source.assign(1024, 'x');
		}
		Require(original == "03 Lighting/Opaque/Shaders/Lighting.shader"_h && node->GetGpuTimingName(3) == original,
			"GPU labels must preserve their index, node tag, queue tag and shader after source destruction");
		node->SetFloat("gain"_h, 2.0f);
		node->SetString("label"_h, "unrelated");
		Require(node->GetGpuTimingName(3) == original, "unrelated parameters must not change the timing identity");
		node->SetString("Tag"_h, "Transparent");
		Require(node->GetGpuTimingName(3) == "03 Lighting/Transparent/Shaders/Lighting.shader"_h,
			"changing the render queue tag must update the timing label");
		node->SetString("shader"_h, "Shaders/Unlit.shader");
		Require(node->GetGpuTimingName(3) == "03 Lighting/Transparent/Shaders/Unlit.shader"_h,
			"changing the shader must update the timing label");
		node->SetTag("PostProcess"_h);
		Require(node->GetGpuTimingName(3) == "03 PostProcess/Transparent/Shaders/Unlit.shader"_h,
			"changing the node tag must update the timing label");
		Require(node->GetGpuTimingName(103) == "103 PostProcess/Transparent/Shaders/Unlit.shader"_h &&
			node->GetGpuTimingName(3) == "03 PostProcess/Transparent/Shaders/Unlit.shader"_h,
			"moving or sharing a node must preserve the current graph position in its timing label");
		node->SetString("Tag"_h, {});
		node->SetString("shader"_h, {});
		Require(node->GetGpuTimingName(3) == "03 PostProcess"_h, "empty optional labels must not leave separators");
		node.Clear();
		Require(original.ToString() == "03 Lighting/Opaque/Shaders/Lighting.shader",
			"pending queries must retain readable labels after the node is changed or destroyed");
	}

	void TestBorrowedAssetText()
	{
		const char extent[] = { ' ', '+', '1', '7', ' ', 'X' };
		Require(FrameGraphAsset::RenderTarget::ParseUintValue(std::string_view(extent, 5)) == 17,
			"dimension parsing must accept bounded text without reading a suffix or requiring a terminator");
		bool rejected = false;
		try { FrameGraphAsset::RenderTarget::ParseUintValue({}); }
		catch (const YAML::Exception&) { rejected = true; }
		Require(rejected, "an empty dimension view must fail parsing");

		FrameGraphAsset::Value value;
		{
			std::string source = "prefix:Transparent:suffix";
			value = FrameGraphAsset::Value(std::string_view(source).substr(7, 11));
			source.assign(1024, 'x');
		}
		Require(value.IsString() && value.GetString() == "Transparent",
			"an authored value must own the text supplied through its view after the source changes or dies");
	}

	void TestResourceDeclarations()
	{
		for (const char* document : {
			"samplers: [{name: Shared, path: first.png, fileId: ''}, {name: Shared, path: second.png, fileId: ''}]",
			"renderTargets: [{name: Shared, width: 8}, {name: Shared, width: 16}]",
			"samplers: [{name: Shared, path: first.png, fileId: ''}]\nrenderTargets: [{name: Shared, width: 8}]" })
		{
			FrameGraphAsset asset;
			bool rejected = false;
			try { asset.Deserialize(YAML::Load(document)); }
			catch (const YAML::Exception& error)
			{
				rejected = true;
				Require(std::string(error.what()).find("Shared") != std::string::npos,
					"an ambiguous resource diagnostic must identify its name");
			}
			Require(rejected, "duplicate static resource declarations must not silently choose a different image");
		}
		std::cout << "FrameGraph duplicate sampler/target declarations rejected\n";
	}

	void TestSamplerReferences()
	{
		bool rejected = false;
		try
		{
			FrameGraphAsset invalid;
			invalid.Deserialize(YAML::Load("samplers: [{name: Missing}]"));
		}
		catch (const YAML::Exception& error)
		{
			rejected = std::string(error.what()).find("Missing") != std::string::npos;
		}
		Require(rejected, "a static sampler without either asset reference must identify the missing source");
		const FileId id("00000000-0000-0000-0000-000000000170");
		FrameGraphAsset asset;
		asset.Deserialize(YAML::Load(R"(
samplers:
  - {name: ByPath, path: Texture.tga}
  - {name: ById, fileId: 00000000-0000-0000-0000-000000000170}
  - {name: Both, fileId: 00000000-0000-0000-0000-000000000170, path: Other.tga}
renderTargets:
  - {name: Color, width: 8, height: 8, format: R32G32B32A32_SFLOAT}
frame:
  - name: PostProcess
    renderTargets:
      - color: Color
      - sourceSampler: ById
      - externalSampler: PublishedLater
)"));
		Require(asset.m_samplers.Num() == 3 && asset.m_renderTargets.Num() == 1,
			"distinct static resource declarations must remain distinct");
		Require(asset.m_samplers["ByPath"].m_path == "Texture.tga" && !asset.m_samplers["ByPath"].m_fileId &&
			asset.m_samplers["ById"].m_path.empty() && asset.m_samplers["ById"].m_fileId == id &&
			asset.m_samplers["Both"].m_fileId == id && asset.m_samplers["Both"].m_path == "Other.tga",
			"samplers must support either asset identity without requiring a redundant path");
		Require(asset.m_nodes.Num() == 1 && asset.m_nodes[0].m_renderTargets["externalSampler"] == "PublishedLater",
			"names supplied by runtime producers must remain legal without a static declaration");

		asset.Deserialize(YAML::Load(R"(
samplers: [{name: ByPath, path: Replacement.tga}]
float: [{gain: 2}]
frame: [{name: Clear}]
)"));
		Require(asset.m_samplers.Num() == 1 && asset.m_samplers["ByPath"].m_path == "Replacement.tga" &&
			asset.m_renderTargets.IsEmpty() && asset.m_nodes.Num() == 1 && asset.m_nodes[0].m_name == "Clear",
			"reading another graph must replace declarations and passes instead of mixing two graphs");
		asset.Deserialize(YAML::Load("{}"));
		Require(asset.m_samplers.IsEmpty() && asset.m_values.IsEmpty() && asset.m_renderTargets.IsEmpty() && asset.m_nodes.IsEmpty(),
			"an empty graph must clear every previous declaration");
		std::cout << "FrameGraph path/id references, external names and replacement passed\n";
	}

	void TestAttachmentDimensions()
	{
		for (const char* field : { "width", "height" })
		{
			for (const char* value : { "0", "-1", "4294967296", "16junk", "1.5", "8/2", "", "Unknown",
				"RenderWidthTypo", "RenderWidth/0", "RenderHeight/-2", "ViewportWidth/garbage",
				"ViewportHeight/2/3", "RenderWidth/1junk", "RenderWidth/nan", "RenderWidth/inf", "RenderWidth/1e-1000" })
			{
				YAML::Node target;
				target["name"] = "InvalidExtent";
				target[field] = value;
				YAML::Node document;
				document["renderTargets"].push_back(target);
				bool rejected = false;
				try { FrameGraphAsset asset; asset.Deserialize(document); }
				catch (const YAML::Exception&) { rejected = true; }
				Require(rejected, "invalid attachment dimensions must fail parsing before any GPU allocation");
			}
		}
		for (const char* value : { "0", "-1", "2147483648", "1.5" })
		{
			bool rejected = false;
			try
			{
				FrameGraphAsset asset;
				asset.Deserialize(YAML::Load(std::string("renderTargets: [{name: InvalidMips, maxMipLevel: '") + value + "'}]"));
			}
			catch (const YAML::Exception&) { rejected = true; }
			Require(rejected, "an invalid mip limit must not produce a zero-level or wrapped allocation");
		}
		FrameGraphAsset asset;
		asset.Deserialize(YAML::Load(R"(
renderTargets:
  - {name: Default}
  - {name: Explicit, width: ' 17 ', height: '+9', maxMipLevel: 4, bGenerateMips: true}
)"));
		Require(asset.m_renderTargets["Default"].m_width == 1 && asset.m_renderTargets["Default"].m_height == 1 &&
			asset.m_renderTargets["Default"].m_maxMipLevel == 10000,
			"omitted dimensions and mip limits must retain their defaults");
		Require(asset.m_renderTargets["Explicit"].m_width == 17 && asset.m_renderTargets["Explicit"].m_height == 9 &&
			asset.m_renderTargets["Explicit"].m_maxMipLevel == 4 && asset.m_renderTargets["Explicit"].m_bGenerateMips,
			"positive dimensions and mip limits must preserve authored values");
		std::cout << "FrameGraph attachment dimensions and mip declarations passed\n";
	}

	void TestNodeResourceBindings()
	{
		FrameGraphAsset asset;
		asset.Deserialize(YAML::Load(R"(
renderTargets:
  - {name: Input, width: 4, height: 8, format: R32_SFLOAT}
samplers: [{name: Sampled, path: Texture.tga}]
frame:
  - name: RenderScene
    renderTargets: [{color: Sampled}, {depthStencil: Input}, {motionVectors: PublishedLater}]
)"));
		const auto& resources = asset.m_nodes[0].m_renderTargets;
		Require(resources.Num() == 3 && resources["color"] == "Sampled" &&
			resources["depthStencil"] == "Input" && resources["motionVectors"] == "PublishedLater",
			"the asset parser must preserve node resource bindings without interpreting their rendering roles");
	}

	void TestGlobalValues(const FrameGraphImporter& importer)
	{
		auto asset = FrameGraphAssetPtr::Make();
		asset->Deserialize(YAML::Load(R"(
float:
  - exposure: 1.75
  - negative: -2.0
  - zero: 0.0
vec4:
  - tint: [0.125, 0.5, 2.0, 0.75]
  - offset: [-3.0, 0.25, 0.0, -1.0]
)"));
		auto first = FrameGraphImporterTestAccess::Build(importer, asset);
		Require(FrameGraphImporterTestAccess::GetValueCount(*first) == 5,
			"building a graph must retain every global value");
		Require(FrameGraphImporterTestAccess::GetValue(*first, "exposure"_h) == glm::vec4(1.75f) &&
			FrameGraphImporterTestAccess::GetValue(*first, "negative"_h) == glm::vec4(-2.0f) &&
			FrameGraphImporterTestAccess::GetValue(*first, "zero"_h) == glm::vec4(0.0f),
			"scalar globals must still broadcast to all four shader components");
		const glm::vec4 tint(0.125f, 0.5f, 2.0f, 0.75f);
		Require(FrameGraphImporterTestAccess::GetValue(*first, "tint"_h) == tint &&
			FrameGraphImporterTestAccess::GetValue(*first, "offset"_h) == glm::vec4(-3.0f, 0.25f, 0.0f, -1.0f),
			"vector globals must retain their individual components, not read the unused scalar field");

		auto second = FrameGraphImporterTestAccess::Build(importer, asset);
		first->SetValue("tint"_h, glm::vec4(4.0f));
		Require(FrameGraphImporterTestAccess::GetValue(*second, "tint"_h) == tint &&
			asset->m_values["tint"].GetVec4() == tint,
			"each instantiated graph must own its values without changing the parsed asset");
		asset->m_values["tint"] = FrameGraphAsset::Value(glm::vec4(8.0f));
		auto third = FrameGraphImporterTestAccess::Build(importer, asset);
		Require(FrameGraphImporterTestAccess::GetValue(*third, "tint"_h) == glm::vec4(8.0f) &&
			FrameGraphImporterTestAccess::GetValue(*second, "tint"_h) == tint,
			"a later build must consume current asset values and retain earlier graph values");

		auto emptyAsset = FrameGraphAssetPtr::Make();
		emptyAsset->Deserialize(YAML::Load("{}"));
		auto empty = FrameGraphImporterTestAccess::Build(importer, emptyAsset);
		Require(FrameGraphImporterTestAccess::GetValueCount(*empty) == 0 && empty->GetGraph().IsEmpty(),
			"an empty description must not inherit values or nodes from previous builds");
	}
}

int main()
{
	try
	{
		Tests::TempDirectory workspace("frame-graph-assets");
		std::filesystem::create_directories(workspace.Path("Content"));
		{
			std::ofstream manifest(workspace.Path("workspace.sailor"));
			manifest <<
				"manifestVersion: 1\n"
				"workspaceId: 00000000-0000-0000-0000-000000000133\n"
				"name: Frame Graph Contract\n"
				"enginePath: .\n"
				"engineReferenceKind: source\n"
				"contentPath: Content\n"
				"sourcePath: Source\n"
				"generatedProjectPath: Generated\n"
				"cachePath: Cache\n"
				"buildPath: Cache/Build\n"
				"logicOutputPath: Binaries\n"
				"logicModuleName: FrameGraphContract\n";
			manifest.close();
			Require(static_cast<bool>(manifest), "the temporary workspace manifest must be written");
		}
		auto resolved = Workspace::ResolveWorkspaceContext(workspace.Get(), workspace.Path("workspace.sailor"));
		Require(resolved.IsSuccess(), "the temporary workspace must resolve");
		AssetRegistry registry(resolved.m_context, nullptr);
		FrameGraphAssetInfoHandler handler(&registry);
		FrameGraphImporter importer(&handler);
		TestRuntimeNamesAndValues();
		TestGpuTimingNames();
		TestBorrowedAssetText();
		TestResourceDeclarations();
		TestSamplerReferences();
		TestAttachmentDimensions();
		TestNodeResourceBindings();
		TestGlobalValues(importer);
		std::cout << "FrameGraphAssetTests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FrameGraphAssetTests failed: " << error.what() << '\n';
		return 1;
	}
}
