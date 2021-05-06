#pragma once
#include <string>
#include "Sailor.h"
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;
namespace Sailor { class UID; }

namespace ns
{
	SAILOR_API void to_json(json& j, const class Sailor::UID& p);
	SAILOR_API void from_json(const json& j, class Sailor::UID& p);
}

namespace Sailor
{
	class UID
	{
	public:

		static SAILOR_API UID CreateNewUID();
		SAILOR_API const std::string& ToString() const;

		SAILOR_API UID() = default;
		SAILOR_API UID(const UID& inUID) = default;
		SAILOR_API void operator=(const UID& inUID);
		SAILOR_API bool operator==(const UID& rhs) const;
		virtual SAILOR_API~UID() = default;

	protected:

		std::string m_UID;

		friend void ns::to_json(json& j, const Sailor::UID& p);
		friend void ns::from_json(const json& j, Sailor::UID& p);
	};
}

namespace std
{
	template<>
	struct hash<Sailor::UID>
	{
		SAILOR_API std::size_t operator()(const Sailor::UID& p) const
		{
			std::hash<std::string> h;
			return h(p.ToString());
		}
	};
}