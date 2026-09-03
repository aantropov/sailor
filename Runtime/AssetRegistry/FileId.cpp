#include "AssetRegistry/FileId.h"

#if defined(_WIN32)
#include <corecrt_io.h>
#include <combaseapi.h>
#else
#include <uuid/uuid.h>
#include <cstdio>
#endif

#include <cctype>

using namespace Sailor;
using namespace nlohmann;

const FileId FileId::Invalid = FileId();

namespace
{
	std::string CanonicalizeFileId(std::string_view value)
	{
		const bool bHasBraces = value.size() == 38 &&
			value.front() == '{' && value.back() == '}';
		if (!bHasBraces && value.size() != 36)
			return std::string(value);

		const size_t offset = bHasBraces ? 1 : 0;
		std::string canonical;
		canonical.reserve(36);
		for (size_t index = 0; index < 36; index++)
		{
			const char character = value[index + offset];
			if (index == 8 || index == 13 || index == 18 || index == 23)
			{
				if (character != '-')
					return std::string(value);
				canonical.push_back(character);
				continue;
			}

			if (!std::isxdigit(static_cast<unsigned char>(character)))
				return std::string(value);

			canonical.push_back(static_cast<char>(
				std::toupper(static_cast<unsigned char>(character))));
		}

		return canonical;
	}
}

FileId::FileId(std::string_view value)
{
	Assign(value);
}

YAML::Node FileId::Serialize() const
{
	YAML::Node outData;
	outData = m_fileId.ToString();
	return outData;
}

void FileId::Deserialize(const YAML::Node& inData)
{
	Assign(inData.as<std::string>());
}

void FileId::Assign(std::string_view value)
{
	m_fileId = StringHash::Runtime(CanonicalizeFileId(value));
}

const std::string& FileId::ToString() const
{
	return m_fileId.ToString();
}

bool FileId::operator==(const FileId& rhs) const
{
	return m_fileId == rhs.m_fileId;
}

FileId FileId::CreateNewFileId()
{
	FileId newuid;
	char buffer[128] = {};

#if defined(_WIN32)
	::GUID win32;
	CoCreateGuid(&win32);
	sprintf_s(buffer, "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
		win32.Data1, win32.Data2, win32.Data3,
		win32.Data4[0], win32.Data4[1], win32.Data4[2], win32.Data4[3],
		win32.Data4[4], win32.Data4[5], win32.Data4[6], win32.Data4[7]);
#else
	uuid_t id;
	uuid_generate(id);
	uuid_unparse_upper(id, buffer);
#endif

	newuid.Assign(buffer);
	return newuid;
}
