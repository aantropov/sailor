#include "Components/Component.h"
#include "Components/LandscapeComponent.h"
#include "Core/Reflection.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace Sailor::Tests
{
	enum class ERecordMode { Persistent, Streamed };

	struct RecordLayer
	{
		FileId m_material{};
		float m_density = 0.5f;
		bool operator==(const RecordLayer&) const = default;
	};

	struct RecordSettings
	{
		std::string m_name = "default";
		uint32_t m_count = 12;
		int32_t m_meshIndex = -1;
		ERecordMode m_mode = ERecordMode::Persistent;
		RecordLayer m_layer{};
		TVector<RecordLayer> m_layers{};
		TVector<ERecordMode> m_modes{ ERecordMode::Persistent, ERecordMode::Streamed };
		bool operator==(const RecordSettings&) const = default;
	};
}

REFL_AUTO(type(Sailor::Tests::RecordLayer),
	field(m_material, Sailor::Attributes::YamlName("materialFileId")),
	field(m_density, Sailor::Attributes::Range(0.0, 1.0)))
REFL_AUTO(type(Sailor::Tests::RecordSettings),
	field(m_name), field(m_count, Sailor::Attributes::Range(0, 2048)), field(m_meshIndex),
	field(m_mode), field(m_layer), field(m_layers), field(m_modes))

namespace Sailor::Tests
{
	class RecordTestComponent final : public Component
	{
		SAILOR_REFLECTABLE(RecordTestComponent)
	public:
		RecordTestComponent() = default;
		const TVector<RecordSettings>& GetRecords() const { return m_records; }
		void SetRecords(const TVector<RecordSettings>& values) { m_records = values; }
	private:
		TVector<RecordSettings> m_records{};
	};
}

REFL_AUTO(type(Sailor::Tests::RecordTestComponent, bases<Sailor::Component>),
	func(GetRecords, property("records")), func(SetRecords, property("records")))

using namespace Sailor;
using namespace Sailor::Tests;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	YAML::Node FindType(const YAML::Node& catalog, std::string_view section, std::string_view name)
	{
		YAML::Node found(YAML::NodeType::Undefined);
		for (const auto& type : catalog[section])
		{
			if (type["typename"].Scalar() != name) continue;
			Require(!found.IsDefined(), "referenced value metadata must occur exactly once");
			found.reset(type);
		}
		Require(found.IsDefined(), "the catalog must include the referenced value type");
		return found;
	}

	void TestReflectedFieldViews()
	{
		TVector<std::string_view> names;
		refl::util::for_each(refl::reflect<RecordLayer>().members, [&](auto member)
			{
				static_assert(std::is_same_v<decltype(GetYamlFieldName(member)), std::string_view>);
				constexpr auto name = GetYamlFieldName(decltype(member){});
				names.Add(name);
			});
		Require(names == TVector<std::string_view>{ "materialFileId", "density" },
			"renamed and prefix-stripped field views must remain valid after reflection descriptors leave scope");
	}

	void TestMetadata(const YAML::Node& catalog)
	{
		for (const auto& type : catalog["engineTypes"])
		{
			const auto& name = type["typename"].Scalar();
			Require(name != "float" && name != "bool" && name != "int" && name != "unsigned int",
				"primitive property types must not be exported as reflected value records");
		}
		const auto root = FindType(catalog, "engineTypes", "Sailor::Tests::RecordTestComponent");
		Require(root["properties"]["records"].Scalar() == "List<Sailor::Tests::RecordSettings>",
			"record lists must use compiler-independent reflected element names");
		const auto settings = FindType(catalog, "engineTypes", "Sailor::Tests::RecordSettings");
		Require(settings["base"].Scalar().empty() && settings["properties"]["count"].Scalar() == "uint32" &&
			settings["properties"]["meshIndex"].Scalar() == "int32" &&
			settings["properties"]["layer"].Scalar() == "Sailor::Tests::RecordLayer" &&
			settings["properties"]["modes"].Scalar() == "List<enum Sailor::Tests::ERecordMode>",
			"record metadata must preserve integer, enum, nested-record and list types");
		const auto layer = FindType(catalog, "engineTypes", "Sailor::Tests::RecordLayer");
		Require(layer["readOnlyProperties"].IsSequence() && layer["readOnlyProperties"].size() == 0,
			"value records without getter-only properties must export an explicit empty sequence");
		Require(layer["properties"]["materialFileId"].Scalar() == "FileId" &&
			layer["propertyRanges"]["density"]["min"].as<float>() == 0 &&
			layer["propertyRanges"]["density"]["max"].as<float>() == 1,
			"metadata must use the same renamed fields and numeric ranges as reflected YAML");
		const auto defaults = FindType(catalog, "cdos", "Sailor::Tests::RecordSettings")["defaultValues"];
		Require(defaults.as<RecordSettings>() == RecordSettings{} && defaults["mode"].Scalar() == "Persistent",
			"record defaults must round-trip through the common YAML serializer with readable enums");
		size_t enumCount = 0;
		for (const auto& group : catalog["enums"])
		{
			if (!group["enum Sailor::Tests::ERecordMode"]) continue;
			const auto values = group["enum Sailor::Tests::ERecordMode"].as<TVector<std::string>>();
			Require(values == TVector<std::string>{ "Persistent", "Streamed" }, "enum metadata must come from the reflected enum");
			++enumCount;
		}
		Require(enumCount == 1, "record and list references must share one enum metadata entry");
	}

	void TestRoundTripAndEdits()
	{
		RecordSettings first;
		first.m_name = "trees";
		first.m_count = 37;
		first.m_layer = { FileId::CreateNewFileId(), 0.75f };
		first.m_layers = { first.m_layer, { FileId::CreateNewFileId(), 0.25f } };
		RecordSettings second = first;
		second.m_name = "grass";
		second.m_count = 1024;
		second.m_mode = ERecordMode::Streamed;
		TVector<RecordSettings> values{ first, second };
		const YAML::Node encoded(values);
		auto decoded = YAML::Load(YAML::Dump(encoded)).as<TVector<RecordSettings>>();
		Require(decoded == values, "record arrays must preserve every field in a YAML round trip");
		std::swap(decoded[0], decoded[1]);
		decoded.RemoveAt(1);
		const YAML::Node edited(decoded);
		Require(edited.as<TVector<RecordSettings>>() == TVector<RecordSettings>{ second },
			"reordering and removing records must keep their fields together");
		RecordTestComponent component;
		component.SetRecords(values);
		auto reflected = component.GetReflectedData().Serialize();
		reflected["overrideProperties"]["records"] = edited;
		ReflectedData replacement;
		replacement.Deserialize(reflected);
		component.ApplyReflection(replacement);
		Require(component.GetRecords() == TVector<RecordSettings>{ second },
			"the ordinary component reflection setter must accept the edited record list");
		component.SetRecords({});
		Require(component.GetReflectedData().Serialize()["overrideProperties"]["records"].as<TVector<RecordSettings>>().IsEmpty(),
			"empty record lists must survive component reflection");
	}

	void TestLandscapeAuthoring(const YAML::Node& catalog)
	{
		const auto componentType = FindType(catalog, "engineTypes", "Sailor::LandscapeComponent")["properties"];
		Require(componentType["vegetationProfiles"].Scalar() == "List<Sailor::LandscapeVegetationSettings>" &&
			componentType["sculptStamps"].Scalar() == "List<Sailor::LandscapeSculptStamp>" &&
			componentType["paintStamps"].Scalar() == "List<Sailor::LandscapePaintStamp>",
			"Landscape authoring must expose complete reflected records, not parallel arrays or float tuples");
		const auto profileType = FindType(catalog, "engineTypes", "Sailor::LandscapeVegetationSettings")["properties"];
		Require(profileType["instancesPerChunk"].Scalar() == "uint32" && profileType["meshIndex"].Scalar() == "int32" &&
			profileType["residency"].Scalar() == "enum Sailor::ELandscapeVegetationResidency" &&
			profileType["shadowMode"].Scalar() == "enum Sailor::ELandscapeVegetationShadowMode",
			"vegetation counts, signed mesh selection and modes must keep their authored types");
		Require(FindType(catalog, "engineTypes", "Sailor::LandscapePaintStamp")["properties"]["layer"].Scalar() == "uint32",
			"paint layers must be reflected as integer indices");

		LandscapeVegetationSettings trees;
		trees.m_modelFileId = FileId::CreateNewFileId();
		trees.m_materialFileId = FileId::CreateNewFileId();
		trees.m_instancesPerChunk = 42;
		trees.m_colliderRadius = 0.5f;
		LandscapeVegetationSettings grass = trees;
		grass.m_modelFileId = FileId::CreateNewFileId();
		grass.m_instancesPerChunk = 1024;
		grass.m_residency = ELandscapeVegetationResidency::Grass;
		grass.m_shadowMode = ELandscapeVegetationShadowMode::None;
		grass.m_meshIndex = 2;
		grass.m_priority = 8;
		LandscapeComponent original;
		original.SetVegetationProfiles({ trees, grass });
		original.SetSculptStamps({ { 1, 2, 3, 4, ELandscapeSculptOperation::Lower } });
		original.SetPaintStamps({ { 4, 3, 2, 1, 2 } });
		const auto yaml = YAML::Load(YAML::Dump(original.GetReflectedData().Serialize()));
		const auto props = yaml["overrideProperties"];
		auto profiles = props["vegetationProfiles"].as<TVector<LandscapeVegetationSettings>>();
		Require(profiles == TVector<LandscapeVegetationSettings>{ trees, grass } &&
			props["sculptStamps"].as<TVector<LandscapeSculptStamp>>() == original.GetSculptStamps() &&
			props["paintStamps"].as<TVector<LandscapePaintStamp>>() == original.GetPaintStamps(),
			"Landscape profile fields and typed stamps must round-trip through the shared YAML serializer");
		Require(grass.m_colliderRadius == profiles[1].m_colliderRadius && !profiles[1].HasCollision(),
			"grass residency must disable collision without erasing the authored collider settings");
		std::swap(profiles[0], profiles[1]);
		profiles.RemoveAt(1);
		auto edited = YAML::Clone(yaml);
		edited["overrideProperties"]["vegetationProfiles"] = profiles;
		ReflectedData data;
		data.Deserialize(edited);
		original.ApplyReflection(data);
		Require(original.GetVegetationProfiles() == TVector<LandscapeVegetationSettings>{ grass },
			"applying a reordered/removed profile must retain all fields of the remaining record");
	}
}

int main(int argc, char** argv)
{
	try
	{
		const auto catalog = Reflection::ExportEngineTypes();
		TestReflectedFieldViews();
		TestMetadata(catalog);
		TestRoundTripAndEdits();
		TestLandscapeAuthoring(catalog);
		if (argc == 2)
		{
			std::ofstream output(argv[1]);
			output << catalog;
			Require(output.good(), "the generated metadata fixture must be writable");
		}
		std::cout << "[PASS] Reflected record metadata, nested types, enums, defaults and component round trips\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
