#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "RHI/Renderer.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "AssetRegistry/Texture/TextureImporter.h"

namespace Sailor
{
	class FrameGraph : public Object
	{
	public:

		SAILOR_API virtual bool IsReady() const override { return true; }

	protected:

		RHIFrameGraphPtr m_frameGraph;
	};

	using FrameGraphPtr = TObjectPtr<FrameGraph>;

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
			std::string m_name;
			float m_float;
			glm::vec4 m_vec4;

			bool operator==(const Value& rhs) const { return m_name == rhs.m_name; }

			virtual void Serialize(nlohmann::json& outData) const { }
			virtual void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();

				if (inData.contains("float"))
				{
					m_float = inData["float"].get<float>();
				}

				if (inData.contains("vec4"))
				{
					m_vec4 = inData["vec4"].get<glm::vec4>();
				}
			}
		};

		class RenderTarget : public IJsonSerializable
		{
		public:

			std::string m_name;
			uint32_t m_width;
			uint32_t m_height;
			//RHI::EFormat m_format;
			std::string m_format;

			bool operator==(const RenderTarget& rhs) const { return m_name == rhs.m_name; }

			virtual void Serialize(nlohmann::json& outData) const { }
			virtual void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();

				if (inData.contains("width"))
				{
					m_width = inData["width"].get<uint32_t>();
				}

				if (inData.contains("height"))
				{
					m_height = inData["height"].get<uint32_t>();
				}

				if (inData.contains("format"))
				{
					m_format = inData["format"].get<std::string>();
				}
			}
		};

		virtual void Serialize(nlohmann::json& outData) const { }
		virtual void Deserialize(const nlohmann::json& inData);

		TMap<std::string, Resource> m_samplers;
		TMap<std::string, Value> m_values;
		TMap<std::string, RenderTarget> m_renderTargets;
		TVector<std::string> m_nodes;
	};

	class SAILOR_API FrameGraphImporter final : public TSubmodule<FrameGraphImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		FrameGraphImporter(FrameGraphAssetInfoHandler* infoHandler);
		virtual ~FrameGraphImporter() override;

		virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		bool LoadFrameGraph_Immediate(UID uid, FrameGraphPtr& outFrameGraph);

		static void RegisterFrameGraphNode(const std::string& nodeName, std::function<FrameGraphNodePtr(void)> factoryMethod);

	protected:

		FrameGraphNodePtr CreateNode(const std::string& nodeName) const;
		static TUniquePtr<TMap<std::string, std::function<FrameGraphNodePtr(void)>>> s_pNodeFactoryMethods;
	};
}
