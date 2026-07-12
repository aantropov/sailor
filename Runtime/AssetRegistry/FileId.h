#pragma once
#include <string>
#include "Core/JsonSerializable.h"
#include "Core/YamlSerializable.h"
#include "Sailor.h"
#include "Core/StringHash.h"

using namespace nlohmann;
namespace Sailor { class FileId; }

#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4251)
#endif

namespace Sailor
{
	class SAILOR_SHARED_API FileId final : public IYamlSerializable
	{
	public:

		static const FileId Invalid;

		static FileId CreateNewFileId();
		const std::string& ToString() const;

		FileId() = default;
		FileId(const FileId& inFileId) = default;
		FileId(FileId&& inFileId) noexcept = default;

		FileId& operator=(const FileId& inFileId) = default;
		FileId& operator=(FileId&& inFileId) noexcept = default;

		__forceinline bool operator==(const FileId& rhs) const;
		__forceinline bool operator!=(const FileId& rhs) const { return !(rhs == *this); }

		explicit operator bool() const { return !m_fileId.IsEmpty() && m_fileId != FileId::Invalid.m_fileId; }

		~FileId() = default;

		virtual YAML::Node Serialize() const override;
		virtual void Deserialize(const YAML::Node& inData) override;

		size_t GetHash() const { return m_fileId.GetHash(); }

	protected:

		StringHash m_fileId = "NullFileId"_h;
	};
}

#if defined(_MSC_VER)
# pragma warning(pop)
#endif
