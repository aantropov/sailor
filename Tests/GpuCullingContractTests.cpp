#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "Raytracing/MaterialUtils.h"
#include "RHI/Texture.h"

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
		Require(raytraceBody.find("sample.m_thicknessFactor > 0.0f") !=
				std::string::npos &&
			raytraceBody.find("IsThickVolumeAtHit") !=
				std::string::npos,
			"primary and ambient thick-volume decisions must use texture-modulated thickness");

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
			thickVolumeBody.find("HasThicknessTexture()") !=
				std::string::npos &&
			thickVolumeBody.find(".g") != std::string::npos &&
			thickVolumeBody.find("GetMaterialData") == std::string::npos,
			"the hit predicate must sample only transmission red and thickness green");
		Require(pathTracerSource.find("material.m_thicknessFactor > 0.0f") ==
				std::string::npos &&
			pathTracerSource.find("hitMaterial.m_thicknessFactor > 0.0f") ==
				std::string::npos,
			"no thick-volume decision may bypass texture-modulated thickness");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "MipExtentUsesVulkanFloorAndClamp", TestMipExtentUsesVulkanFloorAndClamp },
		{ "GpuCullingRangeAndSynchronizationContract", TestGpuCullingRangeAndSynchronizationContract },
		{ "GpuCullingShaderSafetyContract", TestGpuCullingShaderSafetyContract },
		{ "VulkanMemoryBarrierRecordsPipelineBarrier", TestVulkanMemoryBarrierRecordsPipelineBarrier },
		{ "ShaderReadOnlyBarrierSynchronizesShaderSampling", TestShaderReadOnlyBarrierSynchronizesShaderSampling },
		{ "CommandListImageTrackingPreservesPublishedContents", TestCommandListImageTrackingPreservesPublishedContents },
		{ "TransmissionFramebufferMipContract", TestTransmissionFramebufferMipContract },
		{ "TransmissionFramebufferBindingUsesNodeAttachment", TestTransmissionFramebufferBindingUsesNodeAttachment },
		{ "TransparentBackToFrontOrderingContract", TestTransparentBackToFrontOrderingContract },
		{ "PathTracerThicknessSamplerContract", TestPathTracerThicknessSamplerContract },
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
