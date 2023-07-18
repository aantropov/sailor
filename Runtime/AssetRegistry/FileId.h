#pragma once
#include <string>
#include "Core/JsonSerializable.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"

using namespace nlohmann;
namespace Sailor { class FileId; }

namespace Sailor
{
	class FileId final : public IJsonSerializable, public IYamlSerializable
	{
	public:

		static const FileId Invalid;

		SAILOR_API static FileId CreateNewFileId();
		SAILOR_API const std::string& ToString() const;

		SAILOR_API FileId() = default;
		SAILOR_API FileId(const FileId& inFileId) = default;
		SAILOR_API FileId(FileId&& inFileId) = default;

		SAILOR_API FileId& operator=(const FileId& inFileId) = default;
		SAILOR_API FileId& operator=(FileId&& inFileId) = default;

		SAILOR_API __forceinline bool operator==(const FileId& rhs) const;
		SAILOR_API __forceinline bool operator!=(const FileId& rhs) const { return !(rhs == *this); }

		SAILOR_API explicit operator bool() const { return m_fileId != FileId::Invalid.m_fileId; }

		SAILOR_API ~FileId() = default;

		SAILOR_API virtual void Serialize(nlohmann::json& outData) const override;
		SAILOR_API virtual void Deserialize(const nlohmann::json& inData) override;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

	protected:

		std::string m_fileId{};
	};
}

namespace std
{
	template<>
	struct hash<Sailor::FileId>
	{
		SAILOR_API std::size_t operator()(const Sailor::FileId& p) const
		{
			std::hash<std::string> h;
			return h(p.ToString());
		}
	};
}