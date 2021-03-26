#pragma once
#include <string>
#include "nlohmann_json/include/nlohmann/json.hpp"

using namespace nlohmann;
namespace Sailor { class UID; }

namespace ns
{
	void to_json(json& j, const class Sailor::UID& p);
	void from_json(const json& j, class Sailor::UID& p);
}

namespace Sailor
{
	class UID
	{
	public:

		static UID CreateNewUID();
		const std::string& ToString() const;

		UID() = default;
		UID(const UID& inUID) = default;
		void operator=(const UID& inUID);
		virtual ~UID() = default;

	protected:

		std::string uid;

		friend void ns::to_json(json& j, const Sailor::UID& p);
		friend void ns::from_json(const json& j, Sailor::UID& p);
	};
}

namespace std
{
	template<>
	struct hash<Sailor::UID>
	{
		std::size_t operator()(const Sailor::UID& p) const 
		{
			std::hash<std::string> h;
			return h(p.ToString());
		}
	};
}