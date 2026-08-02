#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
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
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "MipExtentUsesVulkanFloorAndClamp", TestMipExtentUsesVulkanFloorAndClamp },
		{ "GpuCullingRangeAndSynchronizationContract", TestGpuCullingRangeAndSynchronizationContract },
		{ "GpuCullingShaderSafetyContract", TestGpuCullingShaderSafetyContract },
		{ "VulkanMemoryBarrierRecordsPipelineBarrier", TestVulkanMemoryBarrierRecordsPipelineBarrier },
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
