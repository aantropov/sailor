#pragma once
#include "AssetInfo.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	/*
			enum class EShaderBindingType : uint8_t
		{
			Float = 0,
			Vec4,
			Bool,
			Texture,
		};

		struct ShaderBinding
		{
			EShaderBindingType m_type = EShaderBindingType::Vec4;
			std::string m_name{};
			uint32_t m_binding = 0;
			uint32_t m_offset = 0;
		};

		std::vector<ShaderBinding> m_bindingLayout;*/


	class ShaderAssetInfo final : public AssetInfo
	{
	public:
		virtual SAILOR_API ~ShaderAssetInfo() = default;
	};

	using ShaderAssetInfoPtr = ShaderAssetInfo*;

	class ShaderAssetInfoHandler final : public TSingleton<ShaderAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		static SAILOR_API void Initialize();

		virtual SAILOR_API void GetDefaultMetaJson(nlohmann::json& outDefaultJson) const;
		virtual SAILOR_API AssetInfoPtr CreateAssetInfo() const;

		virtual SAILOR_API ~ShaderAssetInfoHandler() = default;
	};
}
