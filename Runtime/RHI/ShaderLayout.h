#pragma once

#include "RHI/Types.h"
#include "Core/YamlSerializable.h"
#include "Containers/Set.h"

namespace Sailor::RHI
{
	struct ShaderPushConstantRange
	{
		uint32_t m_offset = 0;
		uint32_t m_size = 0;
	};

	// CPU reflection data. It owns no device resources and can be prepared on a worker.
	struct ShaderStageLayout final : IYamlSerializable
	{
		bool m_bValid = false;
		EShaderStage m_stage = EShaderStage::Vertex;
		TVector<TVector<ShaderLayoutBinding>> m_bindings;
		TVector<ShaderPushConstantRange> m_pushConstants;
		TSet<uint32_t> m_vertexAttributeBindings;
		uint32_t m_fragmentOutputMask = 0;

		SAILOR_API bool Validate(std::string& outDiagnostic) const;
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& data) override;
	};
}
