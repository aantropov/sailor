#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "RHI/Types.h"
#include "AssetRegistry/UID.h"
#include "Core/JsonSerializable.h"

namespace Sailor
{
	class FrameGraphAsset : public IJsonSerializable
	{
	public:

		class Resource : public IJsonSerializable
		{
		public:
			std::string m_name;
			std::string m_path;
			UID m_uid;

			bool operator==(const Resource& rhs) const { return m_name == rhs.m_name; }

			virtual void Serialize(nlohmann::json& outData) const { }
			virtual void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();
				m_path = inData["path"].get<std::string>();
				m_uid.Deserialize(inData["uid"]);
			}
		};

		class Value : public IJsonSerializable
		{
		public:
			std::string m_name{};
			float m_float{};
			glm::vec4 m_vec4{};
			std::string m_string{};

			bool operator==(const Value& rhs) const { return m_name == rhs.m_name; }

			bool IsFloat() const { return  m_bIsFloat; }
			bool IsVec4() const { return   m_bIsVec4; }
			bool IsString() const { return m_bIsString; }

			virtual void Serialize(nlohmann::json& outData) const { }
			virtual void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();

				if (inData.contains("float"))
				{
					m_float = inData["float"].get<float>();
					m_bIsFloat = true;
				}

				if (inData.contains("vec4"))
				{
					m_vec4 = inData["vec4"].get<glm::vec4>();
					m_bIsVec4 = true;
				}

				if (inData.contains("string"))
				{
					m_string = inData["string"].get<std::string>();
					m_bIsString = true;
				}
			}

		protected:

			bool m_bIsFloat = false;
			bool m_bIsVec4 = false;
			bool m_bIsString = false;
		};

		class RenderTarget : public IJsonSerializable
		{
		public:

			std::string m_name;
			uint32_t m_width = 1;
			uint32_t m_height = 1;
			RHI::ETextureFormat m_format;
			bool m_bIsSurface = false;

			bool operator==(const RenderTarget& rhs) const { return m_name == rhs.m_name; }

			virtual void Serialize(nlohmann::json& outData) const { }
			virtual void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();

				if (inData.contains("width"))
				{
					if (inData["width"].is_number())
					{
						m_width = inData["width"].get<uint32_t>();
					}
					else if (inData["width"].get<std::string>() == "ViewportWidth")
					{
						m_width = std::max(1, App::GetViewportWindow()->GetWidth());
					}
				}

				if (inData.contains("height"))
				{
					if (inData["height"].is_number())
					{
						m_height = inData["height"].get<uint32_t>();
					}
					else if (inData["height"].get<std::string>() == "ViewportHeight")
					{
						m_height = std::max(1, App::GetViewportWindow()->GetHeight());
					}
				}

				if (inData.contains("format"))
				{
					DeserializeEnum<RHI::ETextureFormat>(inData["format"], m_format);
				}

				if (inData.contains("bIsSurface"))
				{
					m_bIsSurface = inData["bIsSurface"].get<bool>();
				}
			}
		};

		class Node : public IJsonSerializable
		{
		public:

			std::string m_name;
			TMap<std::string, Value> m_values;
			TMap<std::string, std::string> m_renderTargets;

			virtual void Serialize(nlohmann::json& outData) const { }
			virtual void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();

				if (inData.contains("values"))
				{
					TVector<Value> values;
					DeserializeArray<Value>(values, inData["values"]);

					for (auto& value : values)
					{
						m_values[value.m_name] = std::move(value);
					}
				}

				if (inData.contains("renderTargets"))
				{
					for (auto& target : inData["renderTargets"])
					{
						m_renderTargets[target["name"].get<std::string>()] = target["value"].get<std::string>();
					}
				}
			}
		};

		virtual void Serialize(nlohmann::json& outData) const { }
		virtual void Deserialize(const nlohmann::json& inData);

		TMap<std::string, Resource> m_samplers;
		TMap<std::string, Value> m_values;
		TMap<std::string, RenderTarget> m_renderTargets;
		TVector<Node> m_nodes;
	};

	using FrameGraphAssetPtr = TSharedPtr<FrameGraphAsset>;
}
