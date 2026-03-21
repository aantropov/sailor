#pragma once
#include "Components/Tests/TestCaseComponent.h"

namespace Sailor
{
	class BenchmarkTestCaseComponent : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(BenchmarkTestCaseComponent)

	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;

		SAILOR_API bool GetRunVector() const { return m_bRunVector; }
		SAILOR_API void SetRunVector(bool value) { m_bRunVector = value; }
		SAILOR_API bool GetRunSet() const { return m_bRunSet; }
		SAILOR_API void SetRunSet(bool value) { m_bRunSet = value; }
		SAILOR_API bool GetRunMap() const { return m_bRunMap; }
		SAILOR_API void SetRunMap(bool value) { m_bRunMap = value; }
		SAILOR_API bool GetRunList() const { return m_bRunList; }
		SAILOR_API void SetRunList(bool value) { m_bRunList = value; }
		SAILOR_API bool GetRunOctree() const { return m_bRunOctree; }
		SAILOR_API void SetRunOctree(bool value) { m_bRunOctree = value; }
		SAILOR_API bool GetRunMemory() const { return m_bRunMemory; }
		SAILOR_API void SetRunMemory(bool value) { m_bRunMemory = value; }

	protected:
		bool m_bStarted = false;
		bool m_bRunVector = true;
		bool m_bRunSet = true;
		bool m_bRunMap = true;
		bool m_bRunList = true;
		bool m_bRunOctree = true;
		bool m_bRunMemory = true;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::BenchmarkTestCaseComponent, bases<Sailor::TestCaseComponent>),

	func(GetRunVector, property("runVector"), SkipCDO()),
	func(SetRunVector, property("runVector"), SkipCDO()),
	func(GetRunSet, property("runSet"), SkipCDO()),
	func(SetRunSet, property("runSet"), SkipCDO()),
	func(GetRunMap, property("runMap"), SkipCDO()),
	func(SetRunMap, property("runMap"), SkipCDO()),
	func(GetRunList, property("runList"), SkipCDO()),
	func(SetRunList, property("runList"), SkipCDO()),
	func(GetRunOctree, property("runOctree"), SkipCDO()),
	func(SetRunOctree, property("runOctree"), SkipCDO()),
	func(GetRunMemory, property("runMemory"), SkipCDO()),
	func(SetRunMemory, property("runMemory"), SkipCDO())
)
