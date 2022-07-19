#include "RHIRenderSceneNode.h"
#include "RHI/SceneView.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIRenderSceneNode::m_name = "RenderScene";
#endif

void RHIRenderSceneNode::Initialize(RHIFrameGraphPtr FrameGraph)
{
}

RHI::ESortingOrder RHIRenderSceneNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetStringParam("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

/*
https://developer.nvidia.com/vulkan-shader-resource-binding
*/
void RHIRenderSceneNode::Process(RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	/*
	for each view {
	  bind view resources          // camera, environment...
	  for each shader {
		bind shader pipeline
		bind shader resources      // shader control values
		for each material {
		  bind material resources  // material parameters and textures
		  for each object {
			bind object resources  // object transforms
			draw object
		  }
		}
	  }
	}
	*/
	static TVector<RHIMeshPtr> meshesToRender(64);
	static TVector<RHIMaterialPtr> materialsToRender(64);

	meshesToRender.Clear(false);
	materialsToRender.Clear(false);

	struct DrawMesh
	{
		bool operator==(const DrawMesh& rhs) const { return rhs.m_mesh == m_mesh && m_worldMatrix == rhs.m_worldMatrix; }
		bool operator<(const DrawMesh& rhs) const { return rhs.m_mesh < m_mesh; }

		RHIMeshPtr m_mesh;
		glm::mat4x4 m_worldMatrix;
	};

	static TMap<RHIMaterialPtr, TVector<DrawMesh>> drawCalls(32);
	static TSet<RHIMaterialPtr> materials;
	drawCalls.Clear();

	for (auto& proxy : sceneView.m_proxies)
	{
		for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
		{
			const auto& mesh = proxy.m_meshes[i];
			const auto& material = proxy.GetMaterials()[i];

			if (material->GetRenderState().GetTag() == GetHash(GetStringParam("Tag")))
			{
				DrawMesh meshToDraw;
				meshToDraw.m_mesh = mesh;
				meshToDraw.m_worldMatrix = proxy.m_worldMatrix;

				drawCalls[material].Emplace(std::move(meshToDraw));
				materials.Insert(material);
			}
		}
	}

	for (auto& material : materials)
	{
		drawCalls[material].Sort();
	}
}

void RHIRenderSceneNode::Clear()
{
}