#pragma once
#include "RHI/Types.h"
#include <algorithm>
#include <cstdint>

namespace Sailor::RHI
{
	struct GpuCullingPushConstants
	{
		uint32_t m_numBatches = 0u;
		uint32_t m_numInstances = 0u;
		uint32_t m_firstInstanceIndex = 0u;
		uint32_t m_firstStorageInstance = 0u;
		uint32_t m_firstCandidateInstance = 0u;
		uint32_t m_phase = 0u;
		uint32_t m_bEnableOcclusion = 0u;
	};

	template<typename TCommands>
	void RecordGpuCullingDispatches(
		TCommands& commands,
		RHICommandListPtr commandList,
		RHIShaderPtr shader,
		const TVector<RHIShaderBindingSetPtr>& bindings,
		GpuCullingPushConstants constants,
		uint32_t groupSize,
		bool bDrawOnSameList)
	{
		const EAccessFlags uploadWrites = static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit) |
			static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit);
		const EAccessFlags shaderReadWrite = static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) |
			static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit);
		commands.MemoryBarrier(commandList, uploadWrites, shaderReadWrite);

		constants.m_phase = 0u;
		commands.Dispatch(commandList, shader,
			(std::max)(1u, (constants.m_numInstances + groupSize - 1u) / groupSize),
			1u, 1u, bindings, &constants, sizeof(constants));
		commands.MemoryBarrier(commandList,
			static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), shaderReadWrite);

		constants.m_phase = 1u;
		commands.Dispatch(commandList, shader,
			(std::max)(1u, (constants.m_numBatches + groupSize - 1u) / groupSize),
			1u, 1u, bindings, &constants, sizeof(constants));
		if (bDrawOnSameList)
		{
			const EAccessFlags drawReads = static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) |
				static_cast<EAccessFlags>(EAccessBit::IndirectCommandRead_Bit);
			commands.MemoryBarrier(commandList,
				static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), drawReads);
		}
	}
}
