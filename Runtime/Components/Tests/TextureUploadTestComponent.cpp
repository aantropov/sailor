#include "Components/Tests/TextureUploadTestComponent.h"
#include "Platform/Time.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include <array>
#include <cmath>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	using Pixel = std::array<uint8_t, 4>;
	constexpr std::array<Pixel, 6> FaceColors{
		Pixel{ 255, 17, 31, 63 }, Pixel{ 29, 239, 47, 95 }, Pixel{ 43, 61, 223, 127 },
		Pixel{ 197, 181, 71, 159 }, Pixel{ 83, 167, 149, 191 }, Pixel{ 137, 101, 211, 223 }
	};
	constexpr uint32_t MipCount = 3u;
	const EMemoryPropertyFlags HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;

	RHITexturePtr GetFaceMip(RHITexturePtr cube, uint32_t face, uint32_t mip)
	{
		auto result = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		result->m_vulkan.m_image = cube->m_vulkan.m_image;
		auto view = VulkanImageViewPtr::Make(cube->m_vulkan.m_image->GetDevice(), cube->m_vulkan.m_image);
		view->m_viewType = VK_IMAGE_VIEW_TYPE_2D;
		view->m_subresourceRange.baseArrayLayer = face;
		view->m_subresourceRange.layerCount = 1u;
		view->m_subresourceRange.baseMipLevel = mip;
		view->m_subresourceRange.levelCount = 1u;
		view->Compile();
		result->m_vulkan.m_imageView = view;
		return result;
	}

	std::string ValidateTextures(ShaderSetPtr shader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		std::array<Pixel, 6u * 16u> cubePixels;
		for (uint32_t face = 0u; face < FaceColors.size(); ++face)
			for (uint32_t pixel = 0u; pixel < 16u; ++pixel)
				cubePixels[face * 16u + pixel] = FaceColors[face];
		std::array<Pixel, 16> planePixels;
		for (uint32_t pixel = 0u; pixel < planePixels.size(); ++pixel)
			planePixels[pixel] = Pixel{ uint8_t(13u + 9u * pixel), uint8_t(255u - 7u * pixel),
				uint8_t(5u + 11u * pixel), uint8_t(31u + 13u * pixel) };
		auto cube = driver->CreateTexture(cubePixels.data(), sizeof(cubePixels), glm::ivec3(4, 4, 1),
			MipCount, ETextureType::Cubemap, ETextureFormat::R8G8B8A8_UNORM,
			ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto plane = driver->CreateTexture(planePixels.data(), sizeof(planePixels), glm::ivec3(4, 4, 1),
			1u, ETextureType::Texture2D, ETextureFormat::R8G8B8A8_UNORM,
			ETextureFiltration::Nearest, ETextureClamping::Clamp);

		// Both uploads and these reads use the graphics queue; the readback fence covers them all.
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		std::array<RHIBufferPtr, 2> sampledReadbacks;
		for (uint32_t scenario = 0u; scenario < sampledReadbacks.size(); ++scenario)
		{
			const uint32_t levels = scenario == 0u ? 1u : MipCount;
			auto output = driver->CreateRenderTarget(cmd, glm::ivec2(6, levels), 1u, EFormat::R32G32B32A32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::Storage_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
			auto inputs = driver->CreateShaderBindings();
			if (scenario != 0u)
			{
				driver->AddSamplerToShaderBindings(inputs, "source"_h, cube, 0u);
				commands->ImageMemoryBarrierForComputeSampling(cmd, cube);
			}
			// Empty set 0 exercises the actual missing-sampler fallback, not a replacement texture.
			auto outputs = driver->CreateShaderBindings();
			driver->AddStorageImageToShaderBindings(outputs, "outputValue"_h, output, 0u);
			commands->ImageMemoryBarrier(cmd, output, EImageLayout::ComputeWrite);
			commands->Dispatch(cmd, shader->GetComputeShaderRHI(), 1u, levels, 1u, { inputs, outputs });
			commands->ImageMemoryBarrier(cmd, output, EImageLayout::TransferSrcOptimal);
			sampledReadbacks[scenario] = driver->CreateBuffer(6u * levels * sizeof(glm::vec4),
				EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			commands->CopyImageToBuffer(cmd, output, sampledReadbacks[scenario]);
		}

		commands->ImageMemoryBarrier(cmd, cube, EImageLayout::TransferSrcOptimal);
		std::array<RHIBufferPtr, 6u * MipCount> faceReadbacks;
		for (uint32_t face = 0u; face < FaceColors.size(); ++face)
		{
			for (uint32_t mip = 0u; mip < MipCount; ++mip)
			{
				const uint32_t extent = 4u >> mip;
				auto& readback = faceReadbacks[face * MipCount + mip];
				readback = driver->CreateBuffer(extent * extent * sizeof(Pixel),
					EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				commands->CopyImageToBuffer(cmd, GetFaceMip(cube, face, mip), readback);
			}
		}
		commands->ImageMemoryBarrier(cmd, plane, EImageLayout::TransferSrcOptimal);
		auto planeReadback = driver->CreateBuffer(sizeof(planePixels), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		commands->CopyImageToBuffer(cmd, plane, planeReadback);
		commands->MemoryBarrier(cmd, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(cmd);
		auto fence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(cmd, fence)) return "texture upload readback submission failed";
		fence->Wait(5000000000ull);
		if (!fence->IsFinished()) return "texture upload readback fence exceeded five seconds";

		if (std::memcmp(planeReadback->GetPointer(), planePixels.data(), sizeof(planePixels)) != 0)
			return "ordinary 2D texture upload changed the source pixels";
		for (uint32_t face = 0u; face < FaceColors.size(); ++face)
		{
			for (uint32_t mip = 0u; mip < MipCount; ++mip)
			{
				const uint32_t extent = 4u >> mip;
				const auto* actual = static_cast<const uint8_t*>(faceReadbacks[face * MipCount + mip]->GetPointer());
				for (uint32_t pixel = 0u; pixel < extent * extent; ++pixel)
				{
					for (uint32_t channel = 0u; channel < 4u; ++channel)
					{
						const auto expected = FaceColors[face][channel];
						if (actual[pixel * 4u + channel] != expected)
							return std::format("cube face {} mip {} pixel {} channel {}: expected byte {}, got {}",
								face, mip, pixel, channel, expected, actual[pixel * 4u + channel]);
					}
				}
			}
		}

		const auto linear = [](uint8_t value)
		{
			const float encoded = value / 255.0f;
			return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
		};
		const std::array<float, 4> fallback{ linear(255u), linear(103u), linear(229u), 0.0f };
		for (uint32_t scenario = 0u; scenario < sampledReadbacks.size(); ++scenario)
		{
			const uint32_t levels = scenario == 0u ? 1u : MipCount;
			const auto* actual = static_cast<const float*>(sampledReadbacks[scenario]->GetPointer());
			for (uint32_t mip = 0u; mip < levels; ++mip)
			{
				for (uint32_t face = 0u; face < FaceColors.size(); ++face)
				{
					for (uint32_t channel = 0u; channel < 4u; ++channel)
					{
						const float expected = scenario == 0u ? fallback[channel] : FaceColors[face][channel] / 255.0f;
						const float value = actual[(mip * 6u + face) * 4u + channel];
						if (!std::isfinite(value) || std::abs(value - expected) > 0.002f)
							return std::format("{} sample face {} mip {} channel {}: expected {}, got {}",
								scenario == 0u ? "fallback" : "uploaded cube", face, mip, channel, expected, value);
					}
				}
			}
		}
		return {};
	}
}

void TextureUploadTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const auto& error = m_validation->GetResult();
		if (!error.empty()) { MarkFailed(error); return; }
		AddJournalEvent("TextureUploadEvidence",
			"2D bytes, 6 cube faces at 3 mip levels, 18 uploaded cube samples and 6 missing-sampler sRGB fallback samples passed");
		MarkPassed();
		return;
	}
	if (!m_shader)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		if (auto info = registry->GetAssetInfoPtr("Tests/Shaders/TextureUpload.shader"))
			App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shader);
	}
	if (m_shader && m_shader->IsReady())
	{
		m_validation = Tasks::CreateTaskWithResult<std::string>("Texture upload GPU validation"_h,
			[shader = m_shader]() { return ValidateTextures(shader); }, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("texture upload validation shader did not become ready within 30 seconds");
	}
}
