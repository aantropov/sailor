#include "JsonSerializable.h"

void ns::to_json(nlohmann::json& j, const class Sailor::IJsonSerializable& p)
{
	p.Serialize(j);
}

void ns::from_json(const nlohmann::json& j, class Sailor::IJsonSerializable& p)
{
	p.Deserialize(j);
}
