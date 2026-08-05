#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "Raytracing/MaterialUtils.h"
#include "RHI/Buffer.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include "RHI/Texture.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	class RenderSceneNodeProbe : public Framegraph::RenderSceneNode
	{
	public:
		using TextureBindingCacheKeyProbe = TextureBindingCacheKey;
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	std::string ExtractFunctionBody(const std::string& source, const std::string& signature)
	{
		const size_t signatureOffset = source.find(signature);
		Require(signatureOffset != std::string::npos, "function signature should exist: " + signature);

		const size_t bodyOffset = source.find('{', signatureOffset + signature.size());
		Require(bodyOffset != std::string::npos, "function body should exist: " + signature);

		size_t depth = 0;
		for (size_t offset = bodyOffset; offset < source.size(); ++offset)
		{
			if (source[offset] == '{')
			{
				++depth;
			}
			else if (source[offset] == '}' && --depth == 0)
			{
				return source.substr(bodyOffset, offset - bodyOffset + 1);
			}
		}

		throw std::runtime_error("function body should be balanced: " + signature);
	}

	RHI::RHITexturePtr MakeMipTexture(uint32_t width, uint32_t height, uint32_t baseMipLevel)
	{
		auto image = VulkanImagePtr::Make(VulkanDevicePtr{});
		image->m_extent = { width, height, 1u };
		image->m_mipLevels = baseMipLevel + 1u;
		image->m_arrayLayers = 1u;

		auto imageView = VulkanImageViewPtr::Make(VulkanDevicePtr{}, image);
		imageView->m_subresourceRange.baseMipLevel = baseMipLevel;
		imageView->m_subresourceRange.levelCount = 1u;

		auto texture = RHI::RHITexturePtr::Make(
			RHI::ETextureFiltration::Nearest,
			RHI::ETextureClamping::Clamp,
			true);
		texture->m_vulkan.m_image = image;
		texture->m_vulkan.m_imageView = imageView;
		return texture;
	}

	void TestMipExtentUsesVulkanFloorAndClamp()
	{
		Require(MakeMipTexture(1919u, 1079u, 1u)->GetExtent() == glm::ivec2(959, 539),
			"odd mip dimensions must use Vulkan integer floor semantics");
		Require(MakeMipTexture(3u, 5u, 1u)->GetExtent() == glm::ivec2(1, 2),
			"both odd dimensions must be halved independently");
		Require(MakeMipTexture(3u, 5u, 2u)->GetExtent() == glm::ivec2(1, 1),
			"the final mip must clamp both dimensions to one");
		Require(MakeMipTexture(1u, 720u, 9u)->GetExtent() == glm::ivec2(1, 1),
			"a one-pixel dimension must never become zero");
	}

	void TestGpuCullingRangeAndSynchronizationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string batchHeader = ReadText(sourceRoot / "Runtime/RHI/Batch.hpp");
		const std::string cullingBody = ExtractFunctionBody(batchHeader, "DrawCallStats RHIRecordDrawCallGPUCulling(");

		Require(cullingBody.find("storageIndex[start]") != std::string::npos,
			"a partitioned culling dispatch must start from its requested batch range");
		Require(cullingBody.find("storageIndex[0]") == std::string::npos,
			"a secondary partition must not cull the first partition's instances");
		Require(cullingBody.find("constants.m_phase = 0") != std::string::npos &&
			cullingBody.find("constants.m_phase = 1") != std::string::npos,
			"culling and indirect compaction must be recorded as separate dispatch phases");
		Require(cullingBody.find("EAccessBit::ShaderWrite_Bit") != std::string::npos &&
			cullingBody.find("commands->MemoryBarrier") != std::string::npos,
			"the compaction dispatch must wait for culling shader writes");
		Require(cullingBody.find("m_bEnableOcclusion = 0") != std::string::npos,
			"previous-frame Hi-Z must not cull current-frame transforms");

		const std::string fallbackBody = ExtractFunctionBody(
			batchHeader,
			"DrawCallStats RHIRecordDrawCall(");
		Require(fallbackBody.find(
			"driver->AddBufferToShaderBindings(") != std::string::npos &&
			fallbackBody.find(
				"*indirectCommandBufferBinding") != std::string::npos,
			"the fallback draw path must update the GPU-culling descriptor when its shared indirect buffer grows");
	}

	void TestBatchTextureBindingIdentityContract()
	{
		using TextureBindingCacheKey = RenderSceneNodeProbe::TextureBindingCacheKeyProbe;

		TextureBindingCacheKey firstTextureSet;
		firstTextureSet.m_requestedTextures = { 0u, 4u, 8u };
		TextureBindingCacheKey secondTextureSet;
		secondTextureSet.m_requestedTextures = { 0u, 5u, 8u };

		TMap<TextureBindingCacheKey, uint32_t> textureBindingCache;
		Require(textureBindingCache.Insert(firstTextureSet, 1u),
			"the first texture binding cache key should be inserted");
		Require(textureBindingCache.Insert(secondTextureSet, 2u),
			"a different equal-sized texture binding cache key must not collapse into the first");
		Require(textureBindingCache.Num() == 2,
			"texture sets with the same count and layout capacity must retain distinct cache identities");

		const auto materialBindings = RHI::RHIShaderBindingSetPtr::Make();
		auto material = RHI::RHIMaterialPtr::Make(
			RHI::RenderState{},
			RHI::RHIShaderPtr{},
			RHI::RHIShaderPtr{});
		material->SetBindings(materialBindings);

		const RHI::EBufferUsageFlags bufferUsage =
			RHI::EBufferUsageBit::VertexBuffer_Bit |
			RHI::EBufferUsageBit::IndexBuffer_Bit;
		const auto meshBuffer = RHI::RHIBufferPtr::Make(
			bufferUsage,
			RHI::EMemoryPropertyBit::DeviceLocal);
		auto mesh = RHI::RHIMeshPtr::Make();
		mesh->m_vertexBuffer = meshBuffer;
		mesh->m_indexBuffer = meshBuffer;

		RHI::RHIBatch firstBatch(material, mesh);
		RHI::RHIBatch secondBatch(material, mesh);
		firstBatch.m_textureBindings = RHI::RHIShaderBindingSetPtr::Make();
		secondBatch.m_textureBindings = RHI::RHIShaderBindingSetPtr::Make();
		firstBatch.m_textureBindings->SetVariableDescriptorCount(8u);
		secondBatch.m_textureBindings->SetVariableDescriptorCount(8u);

		TSet<RHI::RHIBatch> batches;
		Require(batches.Insert(firstBatch),
			"the first texture-binding batch should be inserted");
		Require(batches.Insert(secondBatch),
			"a different equal-sized texture-binding batch must not collapse into the first");
		Require(batches.Num() == 2,
			"batch identity must include the texture binding set handle, not its descriptor count");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string batchHeader = ReadText(sourceRoot / "Runtime/RHI/Batch.hpp");
		Require(batchHeader.find("m_textureBindings == rhs.m_textureBindings") != std::string::npos &&
			batchHeader.find("HashCombine(hash, m_textureBindings)") != std::string::npos,
			"render batches must include the texture binding set in equality and hashing");

		const char* drawFunctions[] = {
			"DrawCallStats RHIRecordDrawCallGPUCulling(",
			"DrawCallStats RHIRecordDrawCall(",
			"DrawCallStats RHIDrawCall("
		};

		for (const char* drawFunction : drawFunctions)
		{
			const std::string drawBody = ExtractFunctionBody(batchHeader, drawFunction);
			Require(drawBody.find("prevTextureBindings != vecBatches[j].m_textureBindings") != std::string::npos,
				std::string(drawFunction) +
				" must detect a texture descriptor set change independently of the material pointer");

			const std::string rebindBody = ExtractFunctionBody(
				drawBody,
				"if (bMaterialChanged || bTextureBindingsChanged)");
			Require(rebindBody.find("commands->BindShaderBindings") != std::string::npos &&
				rebindBody.find("prevTextureBindings = vecBatches[j].m_textureBindings") != std::string::npos,
				std::string(drawFunction) +
				" must rebind and remember a different texture descriptor set");
		}
	}

	void TestSceneViewProxyMaterialAlignmentContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string sceneViewSource = ReadText(sourceRoot / "Runtime/RHI/SceneView.cpp");
		const std::string traceBody = ExtractFunctionBody(
			sceneViewSource,
			"TVector<RHISceneViewProxy> RHISceneView::TraceScene(");

		const size_t materialSlotsOffset = traceBody.find(
			"viewProxy.m_overrideMaterials.Resize(viewProxy.m_meshes.Num())");
		const size_t textureSlotsOffset = traceBody.find(
			"viewProxy.m_materialTextureSamplers.Resize(viewProxy.m_meshes.Num())");
		const size_t readyMaterialOffset = traceBody.find(
			"if (material && material->IsReady() && !bSkipMaterials)");
		Require(materialSlotsOffset != std::string::npos &&
			textureSlotsOffset != std::string::npos &&
			readyMaterialOffset != std::string::npos &&
			materialSlotsOffset < readyMaterialOffset &&
			textureSlotsOffset < readyMaterialOffset,
			"stationary scene proxies must allocate one material and texture slot per mesh before testing asynchronous readiness");

		const std::string readyMaterialBody = ExtractFunctionBody(
			traceBody,
			"if (material && material->IsReady() && !bSkipMaterials)");
		Require(readyMaterialBody.find(
			"viewProxy.m_overrideMaterials[i] = material->GetOrAddRHI") != std::string::npos,
			"a ready stationary material must fill its original mesh slot");
		Require(traceBody.find(
			"auto& requestedTextures = viewProxy.m_materialTextureSamplers[i]") != std::string::npos,
			"stationary texture indices must be written into the original mesh slot");

		const std::string depthSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		const std::string depthPrepareBody = ExtractFunctionBody(
			depthSource,
			"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(");
		const std::string pendingMaterialBody = ExtractFunctionBody(
			depthPrepareBody,
			"if (proxy.GetMaterials()[i] == nullptr)");
		Require(pendingMaterialBody.find("continue;") != std::string::npos &&
			pendingMaterialBody.find("break;") == std::string::npos,
			"a pending material slot must not discard later ready meshes from the depth pass");
	}

	void TestModelHierarchyRenderPropagationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string sceneViewSource = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.cpp");
		const std::string traceBody = ExtractFunctionBody(
			sceneViewSource,
			"TVector<RHISceneViewProxy> RHISceneView::TraceScene(");
		Require(traceBody.find("CollectRenderData(") != std::string::npos &&
			traceBody.find("meshProxy.m_worldMatrix * modelMatrix") !=
				std::string::npos &&
			traceBody.find("viewProxy.m_meshModelMatrices.Add(") !=
				std::string::npos,
			"stationary proxies must expand model hierarchy instances into aligned world matrices");

		const std::string staticMeshSource = ReadText(
			sourceRoot / "Runtime/ECS/StaticMeshRendererECS.cpp");
		const std::string collectBody = ExtractFunctionBody(
			staticMeshSource,
			"bool CollectComponentRenderData(");
		Require(collectBody.find("data.GetMeshIndex()") != std::string::npos &&
			collectBody.find("ownerWorldMatrix * modelMatrix") !=
				std::string::npos,
			"static proxies must resolve the selected source mesh and compose its owner transform");
		Require(staticMeshSource.find(
			"proxy.m_meshModelMatrices = std::move(selectedMatrices)") !=
				std::string::npos,
			"cached static proxies must retain one world matrix per selected render part");

		const struct
		{
			const char* m_path;
			const char* m_signature;
			const char* m_label;
		} frameGraphPasses[] = {
			{
				"Runtime/FrameGraph/RenderSceneNode.cpp",
				"Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(",
				"raster"
			},
			{
				"Runtime/FrameGraph/DepthPrepassNode.cpp",
				"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(",
				"depth"
			},
			{
				"Runtime/FrameGraph/ShadowPrepassNode.cpp",
				"void ShadowPrepassNode::Process(",
				"shadow"
			}
		};
		for (const auto& pass : frameGraphPasses)
		{
			const std::string source = ReadText(sourceRoot / pass.m_path);
			const std::string body = ExtractFunctionBody(
				source,
				pass.m_signature);
			Require(body.find("proxy.m_meshModelMatrices[i]") !=
					std::string::npos &&
				body.find("data.model = meshWorldMatrix") !=
					std::string::npos,
				std::string(pass.m_label) +
					" pass must consume the selected render part world matrix");
		}

		const std::string pathTracerEcsSource = ReadText(
			sourceRoot / "Runtime/ECS/PathTracerECS.cpp");
		const std::string pathTracerTick = ExtractFunctionBody(
			pathTracerEcsSource,
			"Tasks::ITaskPtr PathTracerECS::Tick(");
		Require(pathTracerTick.find("pMeshRenderer->GetMeshIndex()") !=
				std::string::npos &&
			pathTracerTick.find("pModel->HasBLAS(meshIndex)") !=
				std::string::npos &&
			pathTracerTick.find("instance.m_meshIndex = meshIndex") !=
				std::string::npos,
			"path-tracer ECS must publish the selected source mesh and its matching BLAS");

		const std::string pathTracerSource = ReadText(
			sourceRoot / "Runtime/Raytracing/PathTracer.cpp");
		Require(pathTracerSource.find(
			"GetBLAS(instance.m_meshIndex)") != std::string::npos &&
			pathTracerSource.find(
				"GetBLASTriangles(instance.m_meshIndex)") !=
				std::string::npos,
			"path tracing must intersect and shade the same selected source-mesh geometry");
	}

	void TestGpuCullingShaderSafetyContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string cullingShader = ReadText(sourceRoot / "Content/Shaders/ComputeMeshCulling.shader");
		const std::string highZShader = ReadText(sourceRoot / "Content/Shaders/ComputeDepthHighZ.shader");

		Require(cullingShader.find("PushConstants.phase == 0") != std::string::npos &&
			cullingShader.find("globalIndex < PushConstants.numBatches") != std::string::npos,
			"the shader must keep culling and compaction in distinct dispatch phases");
		Require(cullingShader.find("column0") != std::string::npos &&
			cullingShader.find("column1") != std::string::npos &&
			cullingShader.find("column2") != std::string::npos,
			"sphere scaling must account for every transformed basis vector");
		Require(cullingShader.find("textureQueryLevels(depthHighZ)") != std::string::npos &&
			cullingShader.find("ceil(log2") != std::string::npos,
			"occlusion LOD selection must cover the projected bounds and clamp to the pyramid");
		Require(cullingShader.find("texelBegin") != std::string::npos &&
			cullingShader.find("texelEnd") != std::string::npos &&
			cullingShader.find("texelFetch") != std::string::npos,
			"occlusion must reduce every mip texel touched by projected bounds");
		Require(highZShader.find("binding = 1, r32f") != std::string::npos,
			"the storage image format must match the R32_SFLOAT Hi-Z render target");
		Require(highZShader.find("greaterThanEqual(pos, outputSize)") != std::string::npos,
			"rounded-up dispatch groups must guard storage-image writes");
		Require(highZShader.find("sourceBegin") != std::string::npos &&
			highZShader.find("sourceEnd") != std::string::npos &&
			highZShader.find("texelFetch") != std::string::npos,
			"odd-sized Hi-Z mips must explicitly reduce their complete source footprint");
	}

	void TestForwardPlusTileSynchronizationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string cullingShader = ReadText(
			sourceRoot / "Content/Shaders/ComputeLightCulling.shader");
		Require(cullingShader.find(
				"const uint offset = tileIndex * LIGHTS_PER_TILE") !=
				std::string::npos,
			"each Forward+ tile must own a deterministic light-list range");
		Require(cullingShader.find("culledLights.indices[0]") ==
				std::string::npos &&
			cullingShader.find("atomicAdd(culledLights") ==
				std::string::npos,
			"Forward+ light-list allocation must not race across workgroups");
		Require(cullingShader.find("const bool isInsideViewport") !=
				std::string::npos &&
			cullingShader.find("if (isInsideViewport)") !=
				std::string::npos,
			"partial Forward+ tiles must not sample outside the viewport");

		const std::string lightCullingSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/LightCullingNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			lightCullingSource,
			"void LightCullingNode::Process(");
		const size_t dispatchOffset = processBody.find("commands->Dispatch(");
		const size_t barrierOffset = processBody.find(
			"commands->MemoryBarrier(",
			dispatchOffset);
		Require(dispatchOffset != std::string::npos &&
			barrierOffset != std::string::npos &&
			processBody.find("EAccessBit::ShaderWrite_Bit", barrierOffset) !=
				std::string::npos &&
			processBody.find("EAccessBit::ShaderRead_Bit", barrierOffset) !=
				std::string::npos,
			"fragment lighting must wait for Forward+ compute shader writes");
		Require(processBody.find("m_boundLightsData != sceneView.m_rhiLightsData") !=
				std::string::npos &&
			processBody.find("m_boundDepthAttachment != depthAttachment") !=
				std::string::npos &&
			processBody.find("m_bindingsViewportSize != pushConstants.m_viewportSize") !=
				std::string::npos,
			"Forward+ buffers must be recreated when their resource identity or extent changes");

		const std::string lightingLibrary = ReadText(
			sourceRoot / "Content/Shaders/Lighting.glsl");
		Require(lightingLibrary.find("uint GetLightTileIndex(") !=
				std::string::npos &&
			lightingLibrary.find("const ivec2 tileId = clamp(") !=
				std::string::npos,
			"Forward+ consumers must clamp screen coordinates to the tile grid");
		Require(lightingLibrary.find(
				"max(roughness * roughness, 0.001)") !=
				std::string::npos &&
			lightingLibrary.find(
				"max(PI * denom * denom, 1e-7)") !=
				std::string::npos,
			"zero-roughness GGX must remain finite");

		for (const std::filesystem::path shaderPath : {
			sourceRoot / "Content/Shaders/Standard.shader",
			sourceRoot / "Content/Shaders/Standard_glTF.shader",
			sourceRoot / "Content/Shaders/Debug.shader" })
		{
			const std::string shader = ReadText(shaderPath);
			Require(shader.find(
					"GetLightTileIndex(gl_FragCoord.xy, frame.viewportSize)") !=
					std::string::npos &&
				shader.find(
					"min(lightsGrid.instance[tileIndex].num, uint(LIGHTS_PER_TILE))") !=
					std::string::npos &&
				shader.find("lightsGrid.instance.length()") !=
					std::string::npos &&
				shader.find("culledLights.indices.length()") !=
					std::string::npos,
				"Forward+ shader must use the shared bounded tile lookup: " +
					shaderPath.generic_string());
		}

		for (const std::filesystem::path shaderPath : {
			sourceRoot / "Content/Shaders/Standard.shader",
			sourceRoot / "Content/Shaders/Standard_glTF.shader" })
		{
			const std::string shader = ReadText(shaderPath);
			Require(shader.find("light.instance.length()") !=
					std::string::npos &&
				shader.find("light.instance[index].type == INVALID_LIGHT_TYPE") !=
					std::string::npos,
				"Forward+ lighting must reject invalid light indices: " +
					shaderPath.generic_string());
		}
	}

	void TestVulkanMemoryBarrierRecordsPipelineBarrier()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string commandBufferSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		const std::string barrierBody = ExtractFunctionBody(
			commandBufferSource,
			"void VulkanCommandBuffer::MemoryBarrier(");

		Require(barrierBody.find("vkCmdPipelineBarrier") != std::string::npos,
			"the RHI buffer barrier must record a Vulkan pipeline barrier");
		Require(barrierBody.find("VkMemoryBarrier") != std::string::npos,
			"the Vulkan barrier must carry the requested access masks");

		const std::string bufferSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanBuffer.cpp");
		Require(bufferSource.find("queues.m_computeFamily.value()") != std::string::npos,
			"concurrently shared culling buffers must include a dedicated compute queue family");
	}

	void TestShaderReadOnlyBarrierSynchronizesShaderSampling()
	{
		const VkAccessFlags shaderReadAccess =
			VulkanCommandBuffer::GetAccessFlags(
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Require((shaderReadAccess & VK_ACCESS_SHADER_READ_BIT) != 0,
			"shader-read image layouts must wait for prior image writes");

		const VkPipelineStageFlags shaderReadStages =
			VulkanCommandBuffer::GetPipelineStage(
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Require((shaderReadStages & VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT) != 0,
			"shader-read image layouts must synchronize graphics shader stages");
	}

	void TestCommandListImageTrackingPreservesPublishedContents()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string driverSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string barrierBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EImageLayout newLayout)");

		Require(barrierBody.find("image->GetDefaultLayout()") !=
			std::string::npos,
			"a new command list must begin from the layout restored by the previous command list");
		Require(barrierBody.find("m_initialLayout") ==
			std::string::npos,
			"published image contents must not be discarded as if every command list were first use");

		const std::string createTextureBody = ExtractFunctionBody(
			driverSource,
			"RHI::RHITexturePtr VulkanGraphicsDriver::CreateTexture(");
		Require(createTextureBody.find("RHI::EImageLayout::Undefined") !=
			std::string::npos &&
			createTextureBody.find("ImageMemoryBarrier") !=
			std::string::npos,
			"empty textures must be initialized before their default layout is assumed");

		const std::string createMsaaBody = ExtractFunctionBody(
			driverSource,
			"RHI::RHITexturePtr VulkanGraphicsDriver::GetOrAddMsaaFramebufferRenderTarget(");
		Require(createMsaaBody.find("defaultLayout") !=
			std::string::npos &&
			createMsaaBody.find("RHI::EImageLayout::Undefined") !=
			std::string::npos &&
			createMsaaBody.find("ImageMemoryBarrier") !=
			std::string::npos,
			"cached MSAA targets must be initialized in their declared attachment layout");
	}

	void TestTransmissionFramebufferMipContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		for (const std::filesystem::path rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadText(rendererPath);
			const size_t secondaryOffset = renderer.find("- name: Secondary");
			const size_t nextTargetOffset = renderer.find(
				"- name:", secondaryOffset + 1u);
			const std::string secondary = renderer.substr(
				secondaryOffset,
				nextTargetOffset - secondaryOffset);
			Require(secondary.find("bGenerateMips: true") !=
				std::string::npos,
				"the transmission snapshot must allocate a mip chain");
			Require(secondary.find("maxMipLevel:") ==
				std::string::npos,
				"the transmission snapshot must retain its complete mip chain");

			const size_t snapshotOffset = renderer.find("- src: Main");
			const size_t transparentOffset = renderer.find(
				"- Tag: Transparent",
				snapshotOffset);
			const std::string snapshot = renderer.substr(
				snapshotOffset > 128u ? snapshotOffset - 128u : 0u,
				transparentOffset -
					(snapshotOffset > 128u ? snapshotOffset - 128u : 0u));
			Require(snapshot.find("GenerateMips: true") !=
				std::string::npos,
				"only the opaque snapshot blit should request mip generation");
		}

		const std::string blitSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/BlitNode.cpp");
		const std::string blitBody = ExtractFunctionBody(
			blitSource,
			"void BlitNode::Process(");
		Require(blitBody.find("TryGetString(\"GenerateMips\"") !=
			std::string::npos &&
			blitBody.find("bResolvedBlitSuccessful") !=
			std::string::npos &&
			blitBody.find("commands->GenerateMipMaps(commandList, dst)") !=
			std::string::npos,
			"snapshot mip generation must be explicit and require a valid base level");

		const std::string shader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(shader.find("textureQueryLevels(g_transmissionFramebufferSampler)") !=
			std::string::npos &&
			shader.find("log2(transmissionFramebufferWidth)") !=
			std::string::npos &&
			shader.find("textureLod(") != std::string::npos &&
			shader.find("transmissionRoughness") != std::string::npos,
			"transmission roughness must select the opaque snapshot mip");
		Require(shader.find("canTransmit") != std::string::npos &&
			shader.find("GetRefractionDirection") != std::string::npos &&
			shader.find("transmissionBrdf") != std::string::npos,
			"transmission must reject total internal reflection independently of volume thickness and use the reflected IBL Fresnel model");
		Require(shader.find("dot(transmissionRay, transmissionRay)") ==
				std::string::npos &&
			shader.find("transmissionRayLength > Epsilon") ==
				std::string::npos,
			"zero-thickness surfaces must retain transmission while volume attenuation remains disabled");
		Require(shader.find("exitEdgeDistance") !=
				std::string::npos &&
			shader.find("environmentRadiance") != std::string::npos &&
			shader.find("framebufferRadiance") != std::string::npos &&
			shader.find("transmissionUvWeight") != std::string::npos,
			"off-screen refraction must smoothly fall back from the opaque framebuffer to environment radiance");
		Require(shader.find("return refractedDirection * thickness * modelScale") !=
				std::string::npos &&
			shader.find("flat vec3 modelScale") != std::string::npos,
			"volume transmission must include runtime instance scale");
		Require(shader.find("NdfGGXAlpha") != std::string::npos &&
			shader.find("VisibilityGGXAlpha") != std::string::npos &&
			shader.find("transmissionFalloff") != std::string::npos,
			"punctual lights must contribute a rough dielectric transmission BTDF");

		const std::string shaderCompilerSource = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Shader/ShaderCompiler.cpp");
		Require(shaderCompilerSource.find("DefaultBoneIdsBinding") !=
				std::string::npos &&
			shaderCompilerSource.find("DefaultBoneWeightsBinding") !=
				std::string::npos,
			"generated shader constants must include the skinned vertex bindings");
		const std::string shaderCompilerHeader = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Shader/ShaderCompiler.h");
		const size_t cacheVersionOffset = shaderCompilerHeader.find(
			"CacheProducerVersion =");
		Require(cacheVersionOffset != std::string::npos &&
			std::stoul(shaderCompilerHeader.substr(
				shaderCompilerHeader.find('=', cacheVersionOffset) + 1u)) >= 6u,
			"the constants cache version must regenerate existing libraries with the skinned bindings");
		const size_t fragmentStage = shader.find("glslFragment: |");
		Require(fragmentStage != std::string::npos &&
			shader.find("BoneMatricesSSBO", fragmentStage) ==
				std::string::npos,
			"the fragment SKINNING permutation must not redeclare vertex-only bone data");

		const std::string commandBufferSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		const std::string generateMipsBody = ExtractFunctionBody(
			commandBufferSource,
			"void VulkanCommandBuffer::GenerateMipMaps(");
		Require(generateMipsBody.find(
			"VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL") !=
			std::string::npos,
			"generated mip levels must end in a shader-readable layout");
		const std::string driverSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string driverMipsBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::GenerateMipMaps(");
		Require(driverMipsBody.find("ShaderReadOnlyOptimal") !=
			std::string::npos,
			"the command-list layout tracker must match mip generation");
	}

	void TestTransmissionFramebufferBindingUsesNodeAttachment()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string frameGraphSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RHIFrameGraph.cpp");
		const std::string frameGraphBody = ExtractFunctionBody(
			frameGraphSource,
			"bool RHIFrameGraph::Process(");
		Require(frameGraphBody.find("GetRenderTarget(\"Secondary\")") ==
			std::string::npos,
			"transmission bindings must not depend on a hardcoded render-target name");
		Require(frameGraphBody.find(
				"node->GetResolvedAttachment(\"transmissionFramebuffer\")") !=
				std::string::npos &&
			frameGraphBody.find("GetDefaultTexture()") !=
				std::string::npos,
			"a graph without a transmission attachment must replace a stale texture binding");

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string renderSceneBody = ExtractFunctionBody(
			renderSceneSource,
			"void RenderSceneNode::Process(");
		Require(renderSceneBody.find(
				"GetResolvedAttachment(\"transmissionFramebuffer\")") !=
				std::string::npos &&
			renderSceneBody.find("g_transmissionFramebufferSampler") !=
				std::string::npos &&
			renderSceneBody.find("AddSamplerToShaderBindings") !=
				std::string::npos,
			"RenderScene must bind the exact transmission attachment that it samples");
	}

	void TestBakedVolumeScalePerInstanceLayoutContract()
	{
		Framegraph::RenderSceneNode::PerInstanceData renderInstance{};
		DepthPrepassNode::PerInstanceData depthInstance{};
		const size_t renderScaleOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(&renderInstance.bakedVolumeScale) -
			reinterpret_cast<const uint8_t*>(&renderInstance));
		const size_t depthScaleOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(&depthInstance.bakedVolumeScale) -
			reinterpret_cast<const uint8_t*>(&depthInstance));

		Require(sizeof(renderInstance) == sizeof(depthInstance) &&
			renderScaleOffset == depthScaleOffset &&
			renderScaleOffset == 96u &&
			sizeof(renderInstance) == 112u &&
			renderScaleOffset + sizeof(vec4) == sizeof(renderInstance),
			"render, depth, and compute passes must share the 112-byte std430 per-instance stride");

		RHI::RHIMesh mesh;
		Require(mesh.m_bakedVolumeScale == glm::vec3(1.0f) &&
			renderInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			depthInstance.bakedVolumeScale == glm::vec4(1.0f),
			"procedural and legacy meshes must default to an identity baked volume scale");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::pair<std::filesystem::path, size_t> sharedShaders[] = {
			{ sourceRoot / "Content/Shaders/ComputeMeshCulling.shader", 1u },
			{ sourceRoot / "Content/Shaders/DepthOnly.shader", 1u },
			{ sourceRoot / "Content/Shaders/Simple.shader", 1u },
			{ sourceRoot / "Content/Shaders/Standard.shader", 2u },
			{ sourceRoot / "Content/Shaders/Standard_glTF.shader", 2u },
			{ sourceRoot / "Content/Shaders/Unlit.shader", 2u }
		};
		for (const auto& [shaderPath, expectedLayoutCount] : sharedShaders)
		{
			const std::string shader = ReadText(shaderPath);
			size_t layoutCount = 0;
			size_t searchOffset = 0;
			while ((searchOffset = shader.find(
				"struct PerInstanceData",
				searchOffset)) != std::string::npos)
			{
				const size_t layoutEnd = shader.find("};", searchOffset);
				Require(layoutEnd != std::string::npos,
					"shared per-instance shader layout must remain balanced: " +
						shaderPath.generic_string());
				const std::string layout = shader.substr(
					searchOffset,
					layoutEnd - searchOffset);
				size_t memberOffset = 0;
				for (const char* member : {
					"mat4 model;",
					"vec4 sphereBounds;",
					"uint materialInstance;",
					"uint skeletonOffset;",
					"uint isCulled;",
					"uint padding;",
					"vec4 bakedVolumeScale;" })
				{
					memberOffset = layout.find(member, memberOffset);
					Require(memberOffset != std::string::npos,
						"shared per-instance shader layouts must preserve the 112-byte std430 member order: " +
							shaderPath.generic_string());
					memberOffset += std::char_traits<char>::length(member);
				}

				++layoutCount;
				searchOffset = layoutEnd + 2u;
			}
			Require(layoutCount == expectedLayoutCount,
				"every shared per-instance shader stage must use the common std430 layout: " +
					shaderPath.generic_string());
		}

		const std::string gltfShader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(gltfShader.find(
				"data.instance[gl_InstanceIndex].bakedVolumeScale.xyz") !=
				std::string::npos,
			"glTF transmission must combine runtime and baked node scale");

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string depthPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		Require(renderSceneSource.find(
				"vec4(mesh->m_bakedVolumeScale, 1.0f)") !=
				std::string::npos &&
			depthPrepassSource.find(
				"vec4(mesh->m_bakedVolumeScale, 1.0f)") !=
				std::string::npos,
			"render and depth passes must upload each RHIMesh baked volume scale");
	}

	void TestDepthPrepassSkinningContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string depthShader = ReadText(
			sourceRoot / "Content/Shaders/DepthOnly.shader");
		Require(depthShader.find("- SKINNING") != std::string::npos &&
			depthShader.find("DefaultBoneIdsBinding") != std::string::npos &&
			depthShader.find("DefaultBoneWeightsBinding") != std::string::npos &&
			depthShader.find("layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO") != std::string::npos &&
			depthShader.find("data.instance[gl_InstanceIndex].skeletonOffset") != std::string::npos &&
			depthShader.find("modelMatrix *= skinMatrix") != std::string::npos,
			"the skinned depth permutation must apply the scene bone palette before projection");

		const std::string depthPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		const std::string getDepthMaterialBody = ExtractFunctionBody(
			depthPrepassSource,
			"RHI::RHIMaterialPtr DepthPrepassNode::GetOrAddDepthMaterial(");
		Require(getDepthMaterialBody.find("m_skinnedDepthOnlyMaterials") != std::string::npos &&
			getDepthMaterialBody.find("defines.Add(\"SKINNING\")") != std::string::npos,
			"DepthPrepass must cache a separate material for the skinned shader permutation");

		const std::string prepareBody = ExtractFunctionBody(
			depthPrepassSource,
			"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(");
		Require(prepareBody.find("proxy.m_skeletonOffset") != std::string::npos &&
			prepareBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding)") != std::string::npos &&
			prepareBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding)") != std::string::npos &&
			prepareBody.find("GetOrAddDepthMaterial(mesh->m_vertexDescription, bSkinned)") != std::string::npos,
			"DepthPrepass must select its skinned material from both the skeleton offset and the concrete mesh vertex layout");
	}

	void TestShadowPrepassSkinningContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string shadowShader = ReadText(
			sourceRoot / "Content/Shaders/ShadowCaster.shader");
		Require(shadowShader.find("- SKINNING") != std::string::npos &&
			shadowShader.find("DefaultBoneIdsBinding") != std::string::npos &&
			shadowShader.find("DefaultBoneWeightsBinding") != std::string::npos &&
			shadowShader.find("layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO") != std::string::npos &&
			shadowShader.find("data.instance[gl_InstanceIndex].skeletonOffset") != std::string::npos &&
			shadowShader.find("modelMatrix *= skinMatrix") != std::string::npos,
			"the skinned shadow permutation must apply the scene bone palette before light projection");

		const std::string shadowPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string getShadowMaterialBody = ExtractFunctionBody(
			shadowPrepassSource,
			"RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(");
		Require(getShadowMaterialBody.find("m_skinnedShadowMaterials_Evsm") != std::string::npos &&
			getShadowMaterialBody.find("m_skinnedShadowMaterials_Pcf") != std::string::npos &&
			getShadowMaterialBody.find("defines.Add(\"SKINNING\")") != std::string::npos,
			"PCF and EVSM shadow passes must cache separate skinned shader permutations");

		const std::string processBody = ExtractFunctionBody(
			shadowPrepassSource,
			"void ShadowPrepassNode::Process(");
		Require(processBody.find("proxy.m_skeletonOffset") != std::string::npos &&
			processBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding)") != std::string::npos &&
			processBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding)") != std::string::npos &&
			processBody.find("GetOrAddShadowMaterial(mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned)") != std::string::npos &&
			processBody.find("sets.Add(sceneView.m_boneMatrices)") != std::string::npos,
			"ShadowPrepass must select skinned batches and bind the current bone matrices");
	}

	void TestVertexDescriptionAttributeIdentityContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string vertexDescriptionSource = ReadText(
			sourceRoot / "Runtime/RHI/VertexDescription.cpp");
		Require(vertexDescriptionSource.find("SetAttributeFormat(m_bits, location, format)") != std::string::npos &&
			vertexDescriptionSource.find("attribute.m_location == location") != std::string::npos,
			"vertex descriptions must identify and query attributes by shader location, not buffer binding");
	}

	void TestGraphicsPipelineAttachmentCacheContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string materialSource = ReadText(
			sourceRoot / "Runtime/RHI/Material.cpp");
		const std::string getPipelineBody = ExtractFunctionBody(
			materialSource,
			"GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr RHIMaterial::Vulkan::GetOrAddPipeline(");
		Require(getPipelineBody.find("ComputeAspectFlagsForFormat(depthStencilAttachment)") != std::string::npos &&
			getPipelineBody.find("Fits(colorAttachments, depthStencilAttachment, stencilAttachmentFormat)") != std::string::npos,
			"graphics pipeline lookup must compare a depth-only attachment with an undefined stencil format");
		Require(getPipelineBody.find("m_pipelinesLock.Lock()") != std::string::npos &&
			getPipelineBody.find("m_pipelinesLock.Unlock()") != std::string::npos,
			"parallel command-list recording must serialize graphics pipeline cache misses");

		const std::string pipelineStatesSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanPipileneStates.cpp");
		const std::string buildPipelineBody = ExtractFunctionBody(
			pipelineStatesSource,
			"const TVector<VulkanPipelineStatePtr>& VulkanPipelineStateBuilder::BuildPipeline(");
		Require(buildPipelineBody.find("vertexDescription->GetVertexStride()") != std::string::npos &&
			buildPipelineBody.find("vertexDescription->GetAttributeDescriptions()") != std::string::npos &&
			buildPipelineBody.find("vertexAttributeBindings.ToVector()") != std::string::npos &&
			buildPipelineBody.find("orderedVertexAttributeBindings.Sort()") != std::string::npos,
			"pipeline-state cache identity must include the concrete vertex layout and shader input locations");

		const std::string pipelineSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanPipeline.cpp");
		const std::string commandBufferSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		Require(pipelineSource.find("result != VK_SUCCESS || m_pipeline == VK_NULL_HANDLE") != std::string::npos &&
			commandBufferSource.find("if (!m_bGraphicsPipelineBound)") != std::string::npos,
			"failed graphics pipeline compilation must not record draw commands without a valid pipeline");
	}

	void TestPostProcessPingPongMipIsolationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string motionBlur = ReadText(
			sourceRoot / "Content/Shaders/MotionBlur.shader");
		const size_t firstBaseMipSample = motionBlur.find(
			"textureLod(colorSampler");
		const size_t secondBaseMipSample = motionBlur.find(
			"textureLod(colorSampler",
			firstBaseMipSample == std::string::npos ? 0u : firstBaseMipSample + 1u);
		Require(firstBaseMipSample != std::string::npos &&
			secondBaseMipSample != std::string::npos &&
			motionBlur.find("texture(colorSampler") == std::string::npos,
			"motion blur must sample only the tone-mapped base mip after Secondary was used as an HDR transmission snapshot");

		const std::string eyeAdaptationSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/EyeAdaptationNode.cpp");
		const std::string eyeAdaptationBody = ExtractFunctionBody(
			eyeAdaptationSource,
			"void EyeAdaptationNode::Process(");
		Require(eyeAdaptationBody.find("GetMipLayer(0)") !=
				std::string::npos &&
			eyeAdaptationBody.find(
				"TVector<RHI::RHITexturePtr>{colorTarget}") !=
				std::string::npos,
			"eye adaptation must overwrite Secondary through its base-mip attachment view");

		for (const std::filesystem::path rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadText(rendererPath);
			const size_t motionBlurOffset = renderer.find(
				"- shader: Shaders/MotionBlur.shader");
			const size_t debugDrawOffset = renderer.find(
				"- name: DebugDraw",
				motionBlurOffset);
			const size_t outputBlitOffset = renderer.find(
				"- name: Blit",
				debugDrawOffset);
			const size_t renderImGuiOffset = renderer.find(
				"- name: RenderImGui",
				outputBlitOffset);
			Require(motionBlurOffset != std::string::npos &&
				debugDrawOffset != std::string::npos &&
				outputBlitOffset != std::string::npos &&
				renderImGuiOffset != std::string::npos,
				"the post-process, debug overlay, and final output sequence must be explicit");

			const std::string debugDraw = renderer.substr(
				debugDrawOffset,
				outputBlitOffset - debugDrawOffset);
			Require(debugDraw.find("- color: Main") !=
					std::string::npos,
				"DebugDraw must use the HDR-compatible Main surface after MotionBlur");

			const std::string outputBlit = renderer.substr(
				outputBlitOffset,
				renderImGuiOffset - outputBlitOffset);
			Require(outputBlit.find("- src: Main") !=
					std::string::npos &&
				outputBlit.find("- dst: EditorOutput") !=
					std::string::npos,
				"the final blit must preserve the post-MotionBlur debug overlay");
		}
	}

	void TestTransparentBackToFrontOrderingContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		for (const std::filesystem::path rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadText(rendererPath);
			const size_t transparentOffset = renderer.find(
				"- Tag: Transparent");
			Require(transparentOffset != std::string::npos,
				"the renderer must contain a Transparent scene pass");
			const size_t renderTargetsOffset = renderer.find(
				"renderTargets:",
				transparentOffset);
			Require(renderTargetsOffset != std::string::npos,
				"the Transparent pass must declare render targets");
			const std::string settings = renderer.substr(
				transparentOffset,
				renderTargetsOffset - transparentOffset);
			Require(settings.find("- Sorting: BackToFront") !=
					std::string::npos,
				"transparent rendering must explicitly request back-to-front sorting");
			Require(settings.find("- GPUCulling: false") !=
					std::string::npos,
				"the ordered Transparent path must not use order-destroying GPU batch compaction");
		}

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string sortingBody = ExtractFunctionBody(
			renderSceneSource,
			"RHI::ESortingOrder RenderSceneNode::GetSortingOrder() const");
		Require(sortingBody.find("TryGetString(\"Sorting\"") !=
				std::string::npos &&
			sortingBody.find("\n\tGetString(\"Sorting\"") ==
				std::string::npos,
			"render nodes without an explicit sorting parameter must retain the default order safely");
		const std::string prepareBody = ExtractFunctionBody(
			renderSceneSource,
			"Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(");
		Require(prepareBody.find("m_orderedDrawItems.Emplace") !=
				std::string::npos &&
			prepareBody.find("GetViewMatrix()") !=
				std::string::npos &&
			prepareBody.find("-viewCenter.z") !=
				std::string::npos,
			"each transparent mesh instance must retain camera-space center depth");
		Require(prepareBody.find("m_orderedDrawItems.Sort") !=
				std::string::npos &&
			prepareBody.find("lhs.m_cameraDepth > rhs.m_cameraDepth") !=
				std::string::npos &&
			prepareBody.find("lhs.m_staticMeshEcs < rhs.m_staticMeshEcs") !=
				std::string::npos &&
			prepareBody.find("lhs.m_meshIndex < rhs.m_meshIndex") !=
				std::string::npos,
			"transparent instances must use deterministic far-to-near ordering");

		const std::string orderedBody = ExtractFunctionBody(
			renderSceneSource,
			"void RenderSceneNode::ProcessBackToFront(");
		Require(orderedBody.find("gpuMatricesData.Add(item.m_instanceData)") !=
				std::string::npos &&
			orderedBody.find("command.m_instanceCount = 1u") !=
				std::string::npos &&
			orderedBody.find("firstStorageInstance + i") !=
				std::string::npos,
			"the instance SSBO and indirect commands must share sorted indices");
		Require(orderedBody.find("firstItem.m_batch == nextItem.m_batch") !=
				std::string::npos &&
			orderedBody.find("commands->DrawIndexedIndirect") !=
				std::string::npos,
			"only adjacent compatible transparent items may be combined into an MDI run");
		Require(orderedBody.find("BeginSecondaryCommandList") ==
				std::string::npos &&
			orderedBody.find("RHIRecordDrawCallGPUCulling") ==
				std::string::npos,
			"back-to-front rendering must remain sequential on the primary command list");

		const std::string processBody = ExtractFunctionBody(
			renderSceneSource,
			"void RenderSceneNode::Process(");
		Require(processBody.find(
				"GetSortingOrder() == RHI::ESortingOrder::BackToFront") !=
				std::string::npos &&
			processBody.find("ProcessBackToFront(") !=
				std::string::npos,
			"the configured sorting order must select the ordered rendering path");
	}

	void TestShadowCasterRenderQueueContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string shadowSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			shadowSource,
			"void ShadowPrepassNode::Process(");

		Require(processBody.find("GetHash(std::string(\"Opaque\"))") !=
				std::string::npos &&
			processBody.find("GetHash(std::string(\"Masked\"))") !=
				std::string::npos &&
			processBody.find("renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag") !=
				std::string::npos,
			"the shadow pass must accept only Opaque and Masked render queues");
		Require(processBody.find("proxy.m_renderQueueTags[i]") !=
				std::string::npos,
			"shadow filtering must use lightweight queue metadata instead of material bindings");

		const std::string sceneViewSource = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.cpp");
		const std::string traceSceneBody = ExtractFunctionBody(
			sceneViewSource,
			"TVector<RHISceneViewProxy> RHISceneView::TraceScene(");
		Require(traceSceneBody.find("m_renderQueueTags.Add") !=
				std::string::npos &&
			traceSceneBody.find("material->GetRenderState().GetTag()") !=
				std::string::npos,
			"shadow traces that skip RHI materials must still publish per-mesh render queues");

		const std::string staticMeshSource = ReadText(
			sourceRoot / "Runtime/ECS/StaticMeshRendererECS.cpp");
		Require(staticMeshSource.find("proxy.m_renderQueueTags.Add(material->GetRenderState().GetTag())") !=
				std::string::npos,
			"cached static proxies must publish the same per-mesh render queues");
	}

	void TestShadowDepthRangeContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string lightingSource = ReadText(
			sourceRoot / "Content/Shaders/Lighting.glsl");

		Require(lightingSource.find("projCoords.xy = projCoords.xy * 0.5 + 0.5;") !=
				std::string::npos,
			"shadow sampling must remap only XY from clip space to texture coordinates");
		Require(lightingSource.find("projCoords = projCoords * 0.5 + 0.5;") ==
				std::string::npos,
			"Vulkan zero-to-one shadow depth must not be remapped as OpenGL depth");
		Require(lightingSource.find("float pcfDepth = texture(shadowMap, projCoords.xy + offset).r;") !=
				std::string::npos &&
			lightingSource.find("texture(shadowMap, projCoords.xy + offset).r * 0.5 + 0.5") ==
				std::string::npos,
			"PCF must compare raw reverse-Z Vulkan depth values");
		Require(lightingSource.find("projCoords.z < 0.0f || projCoords.z > 1.0f") !=
				std::string::npos,
			"shadow sampling must reject coordinates outside Vulkan's zero-to-one depth range");
		Require(lightingSource.find("shadowType == SHADOW_TYPE_NONE") !=
				std::string::npos &&
			lightingSource.find("dot(surfaceNormal, surfaceToLightDirection)") !=
				std::string::npos,
			"directional shadow sampling must skip disabled shadows and derive slope bias from the direction toward the light");
		Require(lightingSource.find("max(slope, EVSM_MIN_SLOPE_BIAS)") !=
				std::string::npos &&
			lightingSource.find("const float biasedDepth =") !=
				std::string::npos &&
			lightingSource.find("exp(EVSM_C1 * biasedDepth)") !=
				std::string::npos &&
			lightingSource.find("-exp(-EVSM_C2 * biasedDepth)") !=
				std::string::npos,
			"both EVSM warps must use the same non-zero receiver depth bias to avoid self-shadowing bands");

		const std::string boundsSource = ReadText(
			sourceRoot / "Runtime/Math/Bounds.cpp");
		const std::string projectionBody = ExtractFunctionBody(
			boundsSource,
			"glm::mat4 Frustum::CalculateOrthoMatrixByView(");
		Require(projectionBody.find("glm::orthoRH_ZO") != std::string::npos &&
			projectionBody.find("glm::orthoRH_NO") == std::string::npos,
			"shadow cascade projection and shader sampling must use the same Vulkan depth range");

		const std::string shadowPassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			shadowPassSource,
			"void ShadowPrepassNode::Process(");
		Require(processBody.find("shadowPass.m_shadowType == EShadowType::EVSM") !=
				std::string::npos &&
			processBody.find("glm::vec4(1.0f, 1.0f, -1.0f, 1.0f)") !=
				std::string::npos,
			"empty reverse-Z EVSM texels must be cleared to the moments encoded for depth zero");
		const std::string finalShadowBarrier =
			"commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);";
		const size_t finalShadowBarrierOffset = processBody.rfind(finalShadowBarrier);
		const size_t blurReleaseOffset = processBody.find("driver->ReleaseTemporaryRenderTarget(blurAttachment);");
		const size_t depthReleaseOffset = processBody.find("driver->ReleaseTemporaryRenderTarget(depthAttachment);");
		Require(finalShadowBarrierOffset != std::string::npos &&
			blurReleaseOffset != std::string::npos &&
			depthReleaseOffset != std::string::npos &&
			blurReleaseOffset < finalShadowBarrierOffset &&
			finalShadowBarrierOffset < depthReleaseOffset,
			"completed PCF and EVSM shadow targets must be published for shader reads in the same graphics command list");

		const std::string standardSource = ReadText(
			sourceRoot / "Content/Shaders/Standard.shader");
		const std::string gltfSource = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(standardSource.find("CalculateDirectionalShadow(") != std::string::npos &&
			gltfSource.find("CalculateDirectionalShadow(") != std::string::npos &&
			standardSource.find("dot(normal, light.direction)") == std::string::npos &&
			gltfSource.find("dot(normal, light.direction)") == std::string::npos,
			"all lit material paths must use the shared reverse-Z shadow and slope-bias calculation");
	}

	void TestPathTracerThicknessSamplerContract()
	{
		Raytracing::Material material{};
		Require(!material.HasThicknessTexture(),
			"a path-tracing material must default to no thickness texture");
		material.m_thicknessIndex = 0;
		material.m_thicknessFactor = 2.0f;
		Require(material.HasThicknessTexture(),
			"a valid thickness texture slot must be detected");

		Raytracing::CombinedSampler2D sampler;
		sampler.Initialize<vec3>(1u, 1u, 3u);
		sampler.SetPixel(0u, 0u, vec3(0.25f, 0.4f, 0.75f));
		const float sampledThickness = material.m_thicknessFactor *
			sampler.Sample<vec3>(vec2(0.5f)).g;
		Require(std::abs(sampledThickness - 0.8f) < 0.0001f,
			"glTF thickness must use the texture's green channel");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string pathTracerSource = ReadText(
			sourceRoot / "Runtime/Raytracing/PathTracer.cpp");
		const std::string materialBuilderBody = ExtractFunctionBody(
			pathTracerSource,
			"bool BuildRaytracingMaterialsFromRuntimeMaterials(");
		Require(materialBuilderBody.find("samplerName == \"thicknessSampler\"") !=
				std::string::npos &&
			materialBuilderBody.find("outMaterial.m_thicknessIndex") !=
				std::string::npos,
			"runtime thickness samplers must be copied into path-tracing materials");

		const std::string materialDataBody = ExtractFunctionBody(
			pathTracerSource,
			"LightingModel::SampledData PathTracer::GetMaterialData(");
		Require(materialDataBody.find("HasThicknessTexture()") !=
				std::string::npos &&
			materialDataBody.find("m_thicknessIndex") !=
				std::string::npos &&
			materialDataBody.find("Sample<vec3>(uv).g") !=
				std::string::npos,
			"path-traced thickness must multiply the factor by the sampled glTF green channel");

		const std::string raytraceBody = ExtractFunctionBody(
			pathTracerSource,
			"vec3 PathTracer::Raytrace(");
		Require(raytraceBody.find("material.m_thicknessFactor > 0.0f") !=
				std::string::npos &&
			raytraceBody.find("sample.m_thicknessFactor > 0.0f") ==
				std::string::npos &&
			raytraceBody.find("IsThickVolumeAtHit") !=
				std::string::npos,
			"thick-volume classification must use the scalar factor while sampled thickness remains available for attenuation");

		const std::string traceSkyBody = ExtractFunctionBody(
			pathTracerSource,
			"vec3 PathTracer::TraceSky(");
		Require(traceSkyBody.find("IsThickVolumeAtHit") !=
				std::string::npos &&
			traceSkyBody.find("GetMaterialData") == std::string::npos,
			"sky traversal must use the lightweight thick-volume hit sampler");

		const std::string thickVolumeBody = ExtractFunctionBody(
			pathTracerSource,
			"bool PathTracer::IsThickVolumeAtHit(");
		Require(thickVolumeBody.find("ResolveHitTextureCoordinates") !=
				std::string::npos &&
			thickVolumeBody.find("HasTransmissionTexture()") !=
				std::string::npos &&
			thickVolumeBody.find("Sample<vec3>") != std::string::npos &&
			thickVolumeBody.find(".r") != std::string::npos &&
			thickVolumeBody.find("HasThicknessTexture()") ==
				std::string::npos &&
			thickVolumeBody.find(".g") == std::string::npos &&
			thickVolumeBody.find("material.m_thicknessFactor > 0.0f") !=
				std::string::npos &&
			thickVolumeBody.find("GetMaterialData") == std::string::npos,
			"the hit predicate must let transmission texture gate the ray without letting thickness texture change the volume type");
	}

	void TestPathTracerMaterialContentRevisionContract()
	{
		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		MaterialPtr material = MaterialPtr::Make(allocator, FileId::Invalid);
		TexturePtr texture = TexturePtr::Make(allocator, FileId::Invalid);

		uint64_t revision = material->GetContentRevision();
		material->SetUniform("material.transmissionFactor", 1.0f);
		Require(material->GetContentRevision() > revision,
			"changing a scalar uniform must advance the material content revision");

		revision = material->GetContentRevision();
		material->SetUniform("material.attenuationColor", glm::vec4(0.9f, 0.6f, 0.1f, 1.0f));
		Require(material->GetContentRevision() > revision,
			"changing a vector uniform must advance the material content revision");

		revision = material->GetContentRevision();
		material->SetSampler("thicknessSampler", texture);
		Require(material->GetContentRevision() > revision,
			"changing a sampler must advance the material content revision");

		revision = material->GetContentRevision();
		material->SetRenderState(RHI::RenderState(
			true,
			true,
			0.0f,
			false,
			RHI::ECullMode::Back,
			RHI::EBlendMode::None,
			RHI::EFillMode::Fill,
			GetHash(std::string("Transparent"))));
		Require(material->GetContentRevision() > revision,
			"changing render state must advance the material content revision");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string pathTracerSource = ReadText(
			sourceRoot / "Runtime/Raytracing/PathTracer.cpp");
		const std::string signatureBody = ExtractFunctionBody(
			pathTracerSource,
			"size_t ComputeMaterialsSignature(");
		Require(signatureBody.find("GetContentRevision()") !=
				std::string::npos,
			"the path-tracing cache signature must include same-pointer material mutations");

		const std::string materialSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Material/MaterialImporter.cpp");
		const std::string hotReloadBody = ExtractFunctionBody(
			materialSource,
			"Tasks::ITaskPtr Material::OnHotReload(");
		Require(hotReloadBody.find("AdvanceContentRevision()") !=
				std::string::npos,
			"dependency hot reloads must invalidate the cached CPU material and texture snapshot");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "MipExtentUsesVulkanFloorAndClamp", TestMipExtentUsesVulkanFloorAndClamp },
		{ "GpuCullingRangeAndSynchronizationContract", TestGpuCullingRangeAndSynchronizationContract },
		{ "BatchTextureBindingIdentityContract", TestBatchTextureBindingIdentityContract },
		{ "SceneViewProxyMaterialAlignmentContract", TestSceneViewProxyMaterialAlignmentContract },
		{ "ModelHierarchyRenderPropagationContract", TestModelHierarchyRenderPropagationContract },
		{ "GpuCullingShaderSafetyContract", TestGpuCullingShaderSafetyContract },
		{ "ForwardPlusTileSynchronizationContract", TestForwardPlusTileSynchronizationContract },
		{ "VulkanMemoryBarrierRecordsPipelineBarrier", TestVulkanMemoryBarrierRecordsPipelineBarrier },
		{ "ShaderReadOnlyBarrierSynchronizesShaderSampling", TestShaderReadOnlyBarrierSynchronizesShaderSampling },
		{ "CommandListImageTrackingPreservesPublishedContents", TestCommandListImageTrackingPreservesPublishedContents },
		{ "TransmissionFramebufferMipContract", TestTransmissionFramebufferMipContract },
		{ "TransmissionFramebufferBindingUsesNodeAttachment", TestTransmissionFramebufferBindingUsesNodeAttachment },
		{ "BakedVolumeScalePerInstanceLayoutContract", TestBakedVolumeScalePerInstanceLayoutContract },
		{ "DepthPrepassSkinningContract", TestDepthPrepassSkinningContract },
		{ "ShadowPrepassSkinningContract", TestShadowPrepassSkinningContract },
		{ "VertexDescriptionAttributeIdentityContract", TestVertexDescriptionAttributeIdentityContract },
		{ "GraphicsPipelineAttachmentCacheContract", TestGraphicsPipelineAttachmentCacheContract },
		{ "PostProcessPingPongMipIsolationContract", TestPostProcessPingPongMipIsolationContract },
		{ "TransparentBackToFrontOrderingContract", TestTransparentBackToFrontOrderingContract },
		{ "ShadowCasterRenderQueueContract", TestShadowCasterRenderQueueContract },
		{ "ShadowDepthRangeContract", TestShadowDepthRangeContract },
		{ "PathTracerThicknessSamplerContract", TestPathTracerThicknessSamplerContract },
		{ "PathTracerMaterialContentRevisionContract", TestPathTracerMaterialContentRevisionContract },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
