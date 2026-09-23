#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/CommandList.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	template<typename T>
	T OpaqueHandle(uintptr_t value)
	{
		if constexpr (std::is_pointer_v<T>)
		{
			return reinterpret_cast<T>(value);
		}
		else
		{
			return static_cast<T>(value);
		}
	}

	class AttachmentView final : public VulkanImageView
	{
	public:
		AttachmentView(uintptr_t identity, VkFormat format, VkSampleCountFlagBits samples) :
			VulkanImageView({}, MakeImage(format, samples))
		{
			// Distinct opaque identities for native metadata; no GPU handles are allocated or submitted.
			m_imageView = OpaqueHandle<VkImageView>(identity);
		}

		~AttachmentView() override
		{
			m_imageView = VK_NULL_HANDLE;
		}

	private:
		static VulkanImagePtr MakeImage(VkFormat format, VkSampleCountFlagBits samples)
		{
			auto image = VulkanImagePtr::Make(VulkanDevicePtr{});
			image->m_format = format;
			image->m_samples = samples;
			image->m_extent = { 640u, 480u, 1u };
			image->m_mipLevels = 1u;
			image->m_arrayLayers = 1u;
			return image;
		}
	};

	VulkanImageViewPtr MakeView(uintptr_t identity, VkFormat format, VkSampleCountFlagBits samples)
	{
		return TRefPtr<AttachmentView>::Make(identity, format, samples);
	}

	VkPhysicalDeviceDepthStencilResolveProperties MakeResolveProperties(VkResolveModeFlags depthModes,
		VkResolveModeFlags stencilModes, bool independentNone, bool independent)
	{
		VkPhysicalDeviceDepthStencilResolveProperties properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
		properties.supportedDepthResolveModes = depthModes;
		properties.supportedStencilResolveModes = stencilModes;
		properties.independentResolveNone = independentNone ? VK_TRUE : VK_FALSE;
		properties.independentResolve = independent ? VK_TRUE : VK_FALSE;
		return properties;
	}

	void CheckNativeAttachmentValues(uint32_t count, VkFormat depthFormat, bool resolve,
		const VkPhysicalDeviceDepthStencilResolveProperties& properties,
		VkResolveModeFlagBits expectedDepthResolve, VkResolveModeFlagBits expectedStencilResolve)
	{
		const glm::vec4 color(0.25f, 0.5f, 0.75f, 0.875f);
		const VulkanRenderPassClearValues clearValues(color, 0.375f, 73u);
		const VkRect2D area{ { 11, 17 }, { 640u, 480u } };
		TVector<VulkanImageViewPtr> colors;
		TVector<VulkanImageViewPtr> resolves;
		const auto samples = resolve ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
		for (uint32_t index = 0u; index < count; ++index)
		{
			colors.Add(MakeView(0x100u + index, VK_FORMAT_R16G16B16A16_SFLOAT, samples));
			if (resolve)
			{
				resolves.Add(MakeView(0x200u + index, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT));
			}
		}
		const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
		const bool hasStencil = depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
		const auto depth = hasDepth ? MakeView(0x300u, depthFormat, samples) : VulkanImageViewPtr{};
		const auto depthResolve = hasDepth && resolve ?
			MakeView(0x400u, depthFormat, VK_SAMPLE_COUNT_1_BIT) : VulkanImageViewPtr{};
		for (bool clear : { false, true })
		{
			for (bool storeDepth : { false, true })
			{
				// This is the attachment setup used directly by VulkanCommandBuffer::BeginRenderPassEx.
				const VulkanRenderingAttachments attachments(colors, resolves, depth, depthResolve,
					clear, clearValues, storeDepth, properties);
				for (VkRenderingFlags flags : { VkRenderingFlags(0), VkRenderingFlags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) })
				{
					const VkRenderingInfo info = attachments.GetRenderingInfo(area, flags);
					Require(info.sType == VK_STRUCTURE_TYPE_RENDERING_INFO && !info.pNext &&
						info.flags == flags && info.layerCount == 1u && info.viewMask == 0u &&
						info.renderArea.offset.x == 11 && info.renderArea.offset.y == 17 &&
						info.renderArea.extent.width == 640u && info.renderArea.extent.height == 480u,
						"native rendering must preserve area, layer count and inline/secondary flags");
					Require(info.colorAttachmentCount == count && (count == 0u || info.pColorAttachments),
						"native rendering must expose exactly the requested MRT count");
					Require(info.pDepthAttachment && (info.pStencilAttachment != nullptr) == hasStencil,
						"native rendering must omit the stencil descriptor for depth-only and color-only passes");
					const auto loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
					for (uint32_t index = 0u; index < count; ++index)
					{
						const auto& attachment = info.pColorAttachments[index];
						Require(attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO && !attachment.pNext &&
							attachment.imageView == *colors[index] && attachment.imageLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL &&
							attachment.loadOp == loadOp && attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE,
							"each MRT must keep its target identity, layout, load and store operations");
						for (uint32_t component = 0u; component < 4u; ++component)
						{
							Require(attachment.clearValue.color.float32[component] == color[component],
								"depth/stencil clear values must not overwrite any color component");
						}
						Require(attachment.resolveMode == (resolve ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE) &&
							attachment.resolveImageView == (resolve ? *resolves[index] : VK_NULL_HANDLE) &&
							attachment.resolveImageLayout == (resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED),
							"each MSAA MRT must resolve to its matching view without reordering or aliasing another slot");
					}

					const auto& nativeDepth = *info.pDepthAttachment;
					Require(nativeDepth.imageView == (hasDepth ? *depth : VK_NULL_HANDLE) &&
						nativeDepth.imageLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL &&
						nativeDepth.loadOp == loadOp &&
						nativeDepth.storeOp == (storeDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE) &&
						nativeDepth.clearValue.depthStencil.depth == 0.375f && nativeDepth.clearValue.depthStencil.stencil == 73u,
						"depth clear/store values must remain independent of color and optional depth presence");
					Require(nativeDepth.resolveMode == (depthResolve ? expectedDepthResolve : VK_RESOLVE_MODE_NONE) &&
						nativeDepth.resolveImageView == (depthResolve ? *depthResolve : VK_NULL_HANDLE) &&
						nativeDepth.resolveImageLayout == (depthResolve ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED),
						"depth resolve must select the supported mode without depending on its store operation");
					if (hasStencil)
					{
						const auto& stencil = *info.pStencilAttachment;
						const bool resolvesStencil = resolve && expectedStencilResolve != VK_RESOLVE_MODE_NONE;
						Require(stencil.imageView == nativeDepth.imageView &&
							stencil.imageLayout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL &&
							stencil.loadOp == loadOp && stencil.storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
							stencil.clearValue.depthStencil.depth == 0.375f && stencil.clearValue.depthStencil.stencil == 73u &&
							stencil.resolveMode == (resolve ? expectedStencilResolve : VK_RESOLVE_MODE_NONE) &&
							stencil.resolveImageView == (resolvesStencil ? *depthResolve : VK_NULL_HANDLE) &&
							stencil.resolveImageLayout == (resolvesStencil ? VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED),
							"stencil resolve must obey the device coupling while keeping clear/store values and the shared target");
					}
				}
			}
		}
	}

	void TestNativeAttachmentValues()
	{
		const auto properties = MakeResolveProperties(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_MIN_BIT,
			VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, true, false);
		for (uint32_t count : { 0u, 1u, 3u })
		{
			for (VkFormat depthFormat : { VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT })
			{
				for (bool resolve : { false, true })
				{
					CheckNativeAttachmentValues(count, depthFormat, resolve, properties, VK_RESOLVE_MODE_MIN_BIT, VK_RESOLVE_MODE_NONE);
				}
			}
		}
	}

	void TestSupportedDepthResolveModes()
	{
		constexpr auto sampleZero = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
		constexpr auto min = VK_RESOLVE_MODE_MIN_BIT;
		constexpr auto none = VK_RESOLVE_MODE_NONE;
		struct ResolveCase
		{
			VkResolveModeFlags m_depthModes;
			VkResolveModeFlags m_stencilModes;
			bool m_independentNone;
			bool m_independent;
			VkResolveModeFlagBits m_depthOnly;
			VkResolveModeFlagBits m_depthWithStencil;
			VkResolveModeFlagBits m_stencil;
		};
		const ResolveCase cases[]
		{
			{ sampleZero, sampleZero, false, false, sampleZero, sampleZero, sampleZero },
			{ sampleZero, sampleZero, true, false, sampleZero, sampleZero, none },
			{ sampleZero, sampleZero, true, true, sampleZero, sampleZero, none },
			{ sampleZero | min, sampleZero, false, false, min, sampleZero, sampleZero },
			{ sampleZero | min, sampleZero, true, false, min, min, none },
			{ sampleZero | min, sampleZero, true, true, min, min, none },
			{ sampleZero | min, sampleZero | min, false, false, min, min, min },
			{ sampleZero | min, sampleZero | min, true, false, min, min, none },
			{ sampleZero | min, sampleZero | min, true, true, min, min, none },
			{ sampleZero, sampleZero | min, false, false, sampleZero, sampleZero, sampleZero },
			{ sampleZero, sampleZero | min, true, false, sampleZero, sampleZero, none },
			{ sampleZero, sampleZero | min, true, true, sampleZero, sampleZero, none },
			{ sampleZero | VK_RESOLVE_MODE_AVERAGE_BIT | VK_RESOLVE_MODE_MAX_BIT,
				sampleZero | VK_RESOLVE_MODE_MAX_BIT, false, false, sampleZero, sampleZero, sampleZero }
		};

		for (const auto& test : cases)
		{
			const auto properties = MakeResolveProperties(test.m_depthModes, test.m_stencilModes,
				test.m_independentNone, test.m_independent);
			for (bool resolve : { false, true })
			{
				CheckNativeAttachmentValues(3u, VK_FORMAT_D32_SFLOAT, resolve, properties, test.m_depthOnly, none);
				for (VkFormat format : { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT })
				{
					CheckNativeAttachmentValues(3u, format, resolve, properties, test.m_depthWithStencil, test.m_stencil);
				}
			}
		}
	}

	void TestDefaultClearValues()
	{
		const auto color = MakeView(1u, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);
		const auto depth = MakeView(2u, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT);
		const auto properties = MakeResolveProperties(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, false, false);
		const VulkanRenderingAttachments attachments({ color }, {}, depth, {}, true, {}, true, properties);
		const auto info = attachments.GetRenderingInfo({ { 0, 0 }, { 1u, 1u } }, 0);
		for (uint32_t component = 0u; component < 4u; ++component)
		{
			Require(info.pColorAttachments[0].clearValue.color.float32[component] == 0.0f,
				"default color clear must remain transparent black");
		}
		Require(info.pDepthAttachment->clearValue.depthStencil.depth == 0.0f &&
			info.pDepthAttachment->clearValue.depthStencil.stencil == 0u,
			"default depth clear must remain reverse-Z far depth with zero stencil");
	}

	class SurfaceForwardingProbe final : public VulkanGraphicsDriver
	{
	public:
		using VulkanGraphicsDriver::BeginRenderPass;
		using VulkanGraphicsDriver::RenderSecondaryCommandBuffers;

		void BeginRenderPass(RHI::RHICommandListPtr cmd, const TVector<RHI::RHITexturePtr>& colors,
			RHI::RHITexturePtr depth, glm::ivec4 area, glm::ivec2 offset, bool clear, glm::vec4 color,
			float clearDepth, bool multisampling, bool storeDepth) override
		{
			m_cmd = cmd;
			m_colors = colors;
			m_depth = depth;
			m_area = area;
			m_offset = offset;
			m_clear = clear;
			m_color = color;
			m_clearDepth = clearDepth;
			m_multisampling = multisampling;
			m_storeDepth = storeDepth;
			++m_calls;
		}

		void RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd, TVector<RHI::RHICommandListPtr> secondary,
			const TVector<RHI::RHITexturePtr>& colors, RHI::RHITexturePtr depth, glm::ivec4 area,
			glm::ivec2 offset, bool clear, glm::vec4 color, float clearDepth, bool multisampling, bool storeDepth) override
		{
			m_secondary = std::move(secondary);
			BeginRenderPass(cmd, colors, depth, area, offset, clear, color, clearDepth, multisampling, storeDepth);
		}

		RHI::RHICommandListPtr m_cmd;
		TVector<RHI::RHICommandListPtr> m_secondary;
		TVector<RHI::RHITexturePtr> m_colors;
		RHI::RHITexturePtr m_depth;
		glm::ivec4 m_area{};
		glm::ivec2 m_offset{};
		glm::vec4 m_color{};
		float m_clearDepth = 0.0f;
		bool m_clear = false;
		bool m_multisampling = false;
		bool m_storeDepth = false;
		uint32_t m_calls = 0u;
	};

	void TestSurfaceOverloadForwarding()
	{
		SurfaceForwardingProbe driver;
		const auto cmd = RHI::RHICommandListPtr::Make(RHI::ECommandListQueue::Graphics);
		const TVector<RHI::RHICommandListPtr> secondary
		{
			RHI::RHICommandListPtr::Make(RHI::ECommandListQueue::Graphics),
			RHI::RHICommandListPtr::Make(RHI::ECommandListQueue::Graphics)
		};
		const auto depth = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, false);
		const glm::ivec4 area(11, 17, 640, 480);
		const glm::ivec2 offset(3, 7);
		const glm::vec4 color(0.25f, 0.5f, 0.75f, 0.875f);
		for (uint32_t count : { 0u, 1u, 3u })
		{
			TVector<RHI::RHISurfacePtr> surfaces;
			TVector<RHI::RHITexturePtr> expectedColors;
			for (uint32_t index = 0u; index < count; ++index)
			{
				const auto target = RHI::RHIRenderTargetPtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, false);
				const auto resolved = RHI::RHIRenderTargetPtr::Make(RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, false);
				surfaces.Add(RHI::RHISurfacePtr::Make(target, resolved, false));
				expectedColors.Add(resolved);
			}
			for (bool clear : { false, true })
			{
				for (bool storeDepth : { false, true })
				{
					for (bool renderSecondary : { false, true })
					{
						const uint32_t previousCalls = driver.m_calls;
						if (renderSecondary)
						{
							driver.RenderSecondaryCommandBuffers(cmd, secondary, surfaces, depth,
								area, offset, clear, color, 0.375f, storeDepth);
							Require(driver.m_secondary == secondary,
								"the actual secondary surface overload must preserve the command list order");
						}
						else
						{
							driver.BeginRenderPass(cmd, surfaces, depth, area, offset, clear, color, 0.375f, storeDepth);
						}
						Require(driver.m_calls == previousCalls + 1u && driver.m_cmd == cmd &&
							driver.m_colors == expectedColors && driver.m_depth == depth &&
							driver.m_area == area && driver.m_offset == offset && driver.m_clear == clear &&
							driver.m_color == color && driver.m_clearDepth == 0.375f,
							"the actual surface overload must forward its resolved MRTs and all render parameters unchanged");
						Require(!driver.m_multisampling && driver.m_storeDepth == storeDepth,
							"a non-resolving surface must disable temporary MSAA without losing or shifting storeDepth");
					}
				}
			}
		}
	}
}

int main()
{
	try
	{
		TestNativeAttachmentValues();
		TestSupportedDepthResolveModes();
		TestDefaultClearValues();
		TestSurfaceOverloadForwarding();
		std::cout << "Vulkan render pass contracts passed.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
