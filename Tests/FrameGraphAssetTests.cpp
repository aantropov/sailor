#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
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

		static glm::vec4 GetValue(const RHI::RHIFrameGraph& graph, const std::string& name)
		{
			const glm::vec4* value = nullptr;
			if (!graph.m_values.Find(name, value))
			{
				throw std::runtime_error("Missing built frame-graph value: " + name);
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
		Require(FrameGraphImporterTestAccess::GetValue(*first, "exposure") == glm::vec4(1.75f) &&
			FrameGraphImporterTestAccess::GetValue(*first, "negative") == glm::vec4(-2.0f) &&
			FrameGraphImporterTestAccess::GetValue(*first, "zero") == glm::vec4(0.0f),
			"scalar globals must still broadcast to all four shader components");
		const glm::vec4 tint(0.125f, 0.5f, 2.0f, 0.75f);
		Require(FrameGraphImporterTestAccess::GetValue(*first, "tint") == tint &&
			FrameGraphImporterTestAccess::GetValue(*first, "offset") == glm::vec4(-3.0f, 0.25f, 0.0f, -1.0f),
			"vector globals must retain their individual components, not read the unused scalar field");

		auto second = FrameGraphImporterTestAccess::Build(importer, asset);
		first->SetValue("tint", glm::vec4(4.0f));
		Require(FrameGraphImporterTestAccess::GetValue(*second, "tint") == tint &&
			asset->m_values["tint"].GetVec4() == tint,
			"each instantiated graph must own its values without changing the parsed asset");
		asset->m_values["tint"] = FrameGraphAsset::Value(glm::vec4(8.0f));
		auto third = FrameGraphImporterTestAccess::Build(importer, asset);
		Require(FrameGraphImporterTestAccess::GetValue(*third, "tint") == glm::vec4(8.0f) &&
			FrameGraphImporterTestAccess::GetValue(*second, "tint") == tint,
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
