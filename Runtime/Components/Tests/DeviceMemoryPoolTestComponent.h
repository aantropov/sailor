#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "Tasks/Tasks.h"

namespace Sailor
{
	class DeviceMemoryPoolTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(DeviceMemoryPoolTestComponent)

	public:
		SAILOR_API void Tick(float deltaTime) override;

	private:
		struct ValidationResult
		{
			std::string m_error;
			std::string m_evidence;
			GraphicsDriver::Vulkan::VulkanBufferPtr m_linearBuffer{};
		};

		struct DepthSnapshot
		{
			GraphicsDriver::Vulkan::VulkanImagePtr m_image{};
			VkExtent2D m_extent{};
			std::string m_error;
			std::string m_evidence;
		};

		Tasks::TaskPtr<ValidationResult> m_validation;
		Tasks::TaskPtr<DepthSnapshot> m_depthSnapshot;
		GraphicsDriver::Vulkan::VulkanBufferPtr m_linearBuffer{};
		GraphicsDriver::Vulkan::VulkanImagePtr m_previousDepth{};
		VkExtent2D m_previousExtent{};
		uint32_t m_resizeCount = 0u;
	};
}

REFL_AUTO(
	type(Sailor::DeviceMemoryPoolTestComponent, bases<Sailor::TestCaseComponent>)
)
