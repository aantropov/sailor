#include "JsonSerializable.h"

void ns::to_json(nlohmann::json& j, const class Sailor::IJsonSerializable& p)
{
	p.Serialize(j);
}

void ns::from_json(const nlohmann::json& j, class Sailor::IJsonSerializable& p)
{
	p.Deserialize(j);
}

void glm::to_json(json& j, const glm::vec2& P)
{
	j = { { "x", P.x }, { "y", P.y } };
}

void glm::from_json(const json& j, glm::vec2& P)
{
	P.x = j.at("x").get<float>();
	P.y = j.at("y").get<float>();
}

void glm::to_json(json& j, const glm::vec3& P)
{
	j = { { "x", P.x }, { "y", P.y }, { "z", P.z } };
}

void glm::from_json(const json& j, glm::vec3& P)
{
	P.x = j.at("x").get<float>();
	P.y = j.at("y").get<float>();
	P.z = j.at("z").get<float>();
}

void glm::to_json(json& j, const glm::vec4& P)
{
	j = { { "x", P.x }, { "y", P.y }, { "z", P.z }, { "w", P.w } };
}

void glm::from_json(const json& j, glm::vec4& P)
{
	P.x = j.at("x").get<float>();
	P.y = j.at("y").get<float>();
	P.z = j.at("z").get<float>();
	P.w = j.at("w").get<float>();
}

void glm::to_json(json& j, const glm::quat& P)
{
	j = { { "x", P.x }, { "y", P.y }, { "z", P.z }, { "w", P.w } };
}

void glm::from_json(const json& j, glm::quat& P)
{
	P.x = j.at("x").get<float>();
	P.y = j.at("y").get<float>();
	P.z = j.at("z").get<float>();
	P.w = j.at("w").get<float>();
}