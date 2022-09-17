#include "DebugDrawNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* DebugDrawNode::m_name = "DebugDraw";
#endif

void DebugDrawNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto colorAttachment = GetResourceParam("color").DynamicCast<RHI::RHISurface>()->GetResolved();
	auto depthAttachment = GetResourceParam("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	commands->ImageMemoryBarrier(commandList, colorAttachment, colorAttachment->GetFormat(), colorAttachment->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	commands->RenderSecondaryCommandBuffers(commandList,
		TVector<RHI::RHICommandListPtr> {sceneView.m_debugDrawSecondaryCmdList},
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		false);

	commands->ImageMemoryBarrier(commandList, colorAttachment, colorAttachment->GetFormat(), EImageLayout::ColorAttachmentOptimal, colorAttachment->GetDefaultLayout());

	/*
	static ShaderSetPtr pComputeShader{};
	static RHI::RHITexturePtr renderTarget{};
	
	if (!pComputeShader)
	{
		auto computeShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeTest.shader");
		App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(computeShaderInfo->GetUID(), pComputeShader);

		RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit | RHI::ETextureUsageBit::Storage_Bit;
		renderTarget = RHI::Renderer::GetDriver()->CreateRenderTarget(glm::vec3(256, 256, 1.0f),
			1, RHI::ETextureType::Texture2D, RHI::ETextureFormat::R32_SFLOAT, RHI::ETextureFiltration::Nearest, RHI::ETextureClamping::Clamp, usage);

		commands->ClearImage(commandList, renderTarget, vec4(0.0f, 0.0f, 0.0f, 1.0f));
	}

	if (pComputeShader->GetComputeShaderRHI())
	{
		RHI::RHIShaderBindingSetPtr computeShaderTest = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		RHI::RHIShaderBindingPtr storageImage = Sailor::RHI::Renderer::GetDriver()->AddStorageImageToShaderBindings(computeShaderTest, "destTex", renderTarget, 0);

		commands->ImageMemoryBarrier(commandList, renderTarget, renderTarget->GetFormat(), renderTarget->GetDefaultLayout(), true);
		commands->Dispatch(commandList, pComputeShader->GetComputeShaderRHI(), { computeShaderTest }, 16, 16, 1);
		commands->ImageMemoryBarrier(commandList, renderTarget, renderTarget->GetFormat(), renderTarget->GetDefaultLayout(), false);

		commands->BlitImage(commandList, renderTarget, colorAttachment, ivec4(0, 0, 256, 256), ivec4(0, 0, 256, 256));
	}*/
}

void DebugDrawNode::Clear()
{
}
