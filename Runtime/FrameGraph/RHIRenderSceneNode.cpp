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

    for (auto& proxy : sceneView.m_proxies)
    {
        for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
        {
            const auto& mesh = proxy.m_meshes[i];
            const auto& material = proxy.GetMaterials()[i];

            if (material->GetRenderState().GetTag() == GetHash(GetStringParam("Tag")))
            {
                meshesToRender.Add(mesh);
                materialsToRender.Add(material);
            }
        }
    }

}

void RHIRenderSceneNode::Clear()
{
}