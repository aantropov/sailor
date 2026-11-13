#pragma once

#include "Containers/Containers.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Sailor::Utils
{
	inline nlohmann::json ParseJsonWithUniqueKeys(std::string_view text)
	{
		bool duplicate = false;
		TVector<TSet<std::string>> objectKeys;
		auto result = nlohmann::json::parse(text, [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value)
		{
			if (event == nlohmann::json::parse_event_t::object_start)
			{
				objectKeys.Add(TSet<std::string>{});
			}
			else if (event == nlohmann::json::parse_event_t::key)
			{
				duplicate |= !objectKeys[objectKeys.Num() - 1].Insert(value.get<std::string>());
			}
			else if (event == nlohmann::json::parse_event_t::object_end)
			{
				objectKeys.RemoveLast();
			}
			return true;
		});
		if (duplicate)
		{
			throw std::invalid_argument("JSON contains duplicate object keys.");
		}
		return result;
	}
}
