#pragma once
#include <string>
#include "Core/JsonSerializable.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"
#include "Core/StringHash.h"

using namespace nlohmann;
namespace Sailor { class FileId; }

namespace Sailor
{
	class FileId final : public IYamlSerializable
	{
	public:

		static const FileId Invalid;

		SAILOR_API static FileId CreateNewFileId();
		SAILOR_API const std::string& ToString() const;

		SAILOR_API FileId() = default;
		SAILOR_API FileId(const FileId& inFileId) = default;
		SAILOR_API FileId(FileId&& inFileId) noexcept = default;

		SAILOR_API FileId& operator=(const FileId& inFileId) = default;
		SAILOR_API FileId& operator=(FileId&& inFileId) noexcept = default;

		SAILOR_API __forceinline bool operator==(const FileId& rhs) const;
		SAILOR_API __forceinline bool operator!=(const FileId& rhs) const { return !(rhs == *this); }

		SAILOR_API explicit operator bool() const { return !m_fileId.IsEmpty() && m_fileId != FileId::Invalid.m_fileId; }

		SAILOR_API ~FileId() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API size_t GetHash() const { return m_fileId.GetHash(); }

	protected:

		StringHash m_fileId = "NullFileId"_h;
	};
}
