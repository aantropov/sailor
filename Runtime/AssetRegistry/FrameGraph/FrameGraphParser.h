#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "RHI/Types.h"
#include "AssetRegistry/FileId.h"
#include "Core/YamlSerializable.h"
#include "Tasks/Scheduler.h"

namespace Sailor
{
	class FrameGraphAsset : public IYamlSerializable
	{
	public:

		class Resource : public IYamlSerializable
		{
		public:
			std::string m_name;
			std::string m_path;
			FileId m_fileId;

			bool operator==(const Resource& rhs) const { return m_name == rhs.m_name; }

			virtual void Deserialize(const YAML::Node& inData)
			{
				m_name = inData["name"].as<std::string>();
				m_path = inData["path"].as<std::string>();
				m_fileId.Deserialize(inData["fileId"]);
			}
		};

		class Value
		{
		public:

			Value() = default;
			Value(float value) : m_bIsFloat(true), m_float(value) {}
			Value(const glm::vec4& value) : m_bIsVec4(true), m_vec4(value) {}
			Value(const std::string& value) : m_bIsString(true), m_string(value) {}

			bool operator==(const Value& rhs) const { return m_name == rhs.m_name; }

			bool IsFloat() const { return  m_bIsFloat; }
			bool IsVec4() const { return   m_bIsVec4; }
			bool IsString() const { return m_bIsString; }

			float GetFloat() const { return m_float; }
			const glm::vec4& GetVec4() const { return m_vec4; }
			const std::string& GetString() const { return m_string; }

		protected:

			std::string m_name{};
			float m_float{ 0.0f };
			glm::vec4 m_vec4{ 0.0f, 0.0f, 0.0f, 1.0f };
			std::string m_string{};

			bool m_bIsFloat = false;
			bool m_bIsVec4 = false;
			bool m_bIsString = false;
		};

		class RenderTarget : public IYamlSerializable
		{
		public:

			std::string m_name;
			uint32_t m_width = 1;
			uint32_t m_height = 1;
			RHI::ETextureFormat m_format{};
			RHI::ETextureFiltration m_filtration = RHI::ETextureFiltration::Linear;
			RHI::ETextureClamping m_clamping = RHI::ETextureClamping::Clamp;
			RHI::ESamplerReductionMode m_reduction = RHI::ESamplerReductionMode::Average;
			bool m_bIsSurface : 1 = false;
			bool m_bIsCompatibleWithComputeShaders : 1 = false;
			bool m_bGenerateMips : 1 = false;
			uint32_t m_maxMipLevel = 10000;

			bool operator==(const RenderTarget& rhs) const { return m_name == rhs.m_name; }

			static uint32_t ParseUintValue(const std::string& str)
			{
				uint32_t res = 1;
				std::stringstream strStream(str);

				if (!(strStream >> res))
				{
					auto strings = Utils::SplitString(str, "/");
					float multiplier = strings.Num() > 1 ? 1.0f / (float)std::atof(strings[1].c_str()) : 1.0f;

					if (str.starts_with("ViewportWidth"))
					{
						res = std::max(1u, (uint32_t)((float)App::GetMainWindow()->GetRenderArea().x * multiplier));
					}
					else if (str.starts_with("ViewportHeight"))
					{
						res = std::max(1u, (uint32_t)((float)App::GetMainWindow()->GetRenderArea().y * multiplier));
					}
					else
					{
						ensure(false, "Variable cannot be parsed");
					}
				}

				return res;
			}

			virtual void Deserialize(const YAML::Node& inData)
			{
				m_name = inData["name"].as<std::string>();

				if (inData["width"])
				{
					m_width = ParseUintValue(inData["width"].as<std::string>());
				}

				if (inData["height"])
				{
					m_height = ParseUintValue(inData["height"].as<std::string>());
				}

				if (inData["format"])
				{
					DeserializeEnum<RHI::ETextureFormat>(inData["format"], m_format);
				}

				if (inData["filtration"])
				{
					DeserializeEnum<RHI::ETextureFiltration>(inData["filtration"], m_filtration);
				}

				if (inData["reduction"])
				{
					DeserializeEnum<RHI::ESamplerReductionMode>(inData["reduction"], m_reduction);
				}

				if (inData["clamping"])
				{
					DeserializeEnum<RHI::ETextureClamping>(inData["clamping"], m_clamping);
				}

				if (inData["bIsSurface"])
				{
					m_bIsSurface = inData["bIsSurface"].as<bool>();
				}

				if (inData["bGenerateMips"])
				{
					m_bGenerateMips = inData["bGenerateMips"].as<bool>();
				}

				if (inData["maxMipLevel"])
				{
					m_maxMipLevel = (uint32_t)inData["maxMipLevel"].as<int32_t>();
				}

				if (inData["bIsCompatibleWithComputeShaders"])
				{
					m_bIsCompatibleWithComputeShaders = inData["bIsCompatibleWithComputeShaders"].as<bool>();
				}
			}
		};

		class Node : public IYamlSerializable
		{
		public:

			std::string m_name;
			std::string m_tag;
			TMap<std::string, Value> m_values;
			TMap<std::string, std::string> m_renderTargets;

			template<typename T>
			void ReadDictionary(const YAML::Node& node)
			{
				for (const auto& p : node)
				{
					for (const auto& keyValue : p)
					{
						const std::string key = keyValue.first.as<std::string>();
						T value = keyValue.second.as<T>();
						m_values[key] = Value(value);
					}
				}
			}

			virtual void Deserialize(const YAML::Node& inData)
			{
				m_name = inData["name"].as<std::string>();

				if (inData["tag"])
				{
					m_tag = inData["tag"].as<std::string>();
				}

				ReadDictionary<std::string>(inData["string"]);
				ReadDictionary<glm::vec4>(inData["vec4"]);
				ReadDictionary<float>(inData["float"]);

				for (const auto& p : inData["renderTargets"])
				{
					for (const auto& keyValue : p)
					{
						const std::string key = keyValue.first.as<std::string>();
						const std::string value = keyValue.second.as<std::string>();

						m_renderTargets[key] = value;
					}
				}
			}
		};

		virtual void Deserialize(const YAML::Node& inData);

		TMap<std::string, Resource> m_samplers;
		TMap<std::string, Value> m_values;
		TMap<std::string, RenderTarget> m_renderTargets;
		TVector<Node> m_nodes;
	};

	using FrameGraphAssetPtr = TSharedPtr<FrameGraphAsset>;
}
