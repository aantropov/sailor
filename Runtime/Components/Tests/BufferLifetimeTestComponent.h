#pragma once
#include "Components/Tests/TestCaseComponent.h"
#include "RHI/Buffer.h"
#include "Tasks/Tasks.h"
#include <array>

namespace Sailor
{
	class BufferLifetimeTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(BufferLifetimeTestComponent)

	public:
		SAILOR_API ~BufferLifetimeTestComponent() override;
		SAILOR_API void Tick(float deltaTime) override;

	private:
		using Allocation = Memory::TManagedMemory<Memory::VulkanBufferMemoryPtr, RHI::RHIBuffer::VulkanBufferAllocator>;
		enum class Phase { Start, InitialUpload, ReplacementUpload, Finished };

		std::string Prepare();
		std::string RecordAndReplace();
		std::string CheckDraws();

		Phase m_phase = Phase::Start;
		std::array<ShaderSetPtr, 2> m_shaders;
		RHI::RHIMeshPtr m_mesh;
		std::array<RHI::RHICommandListPtr, 2> m_draws;
		std::array<RHI::RHIBufferPtr, 2> m_readbacks;
		TWeakPtr<Allocation> m_vertices, m_indices, m_indirect;
		TSharedPtr<RHI::RHIBuffer::VulkanBufferAllocator> m_indirectPool;
		Tasks::TaskPtr<std::string> m_validation;
	};
}

REFL_AUTO(
	type(Sailor::BufferLifetimeTestComponent, bases<Sailor::TestCaseComponent>)
)
