/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* heavily based on CobEngine.cpp */

#include "UnitScriptEngine.h"

#include "CobEngine.h"
#include "CobFileHandler.h"
#include "UnitScript.h"
#include "UnitScriptFactory.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "System/ContainerUtil.h"
#include "System/HashSpec.h"
#include "System/SafeUtil.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#ifdef SYNCCHECK
	#include "System/Sync/SyncChecker.h"
#endif

#include "System/Misc/TracyDefs.h"

CONFIG(bool, AnimationMT).deprecated(true);

static CCobEngine gCobEngine;
static CCobFileHandler gCobFileHandler;
static CUnitScriptEngine gUnitScriptEngine;

CCobEngine* cobEngine = nullptr;
CCobFileHandler* cobFileHandler = nullptr;
CUnitScriptEngine* unitScriptEngine = nullptr;


CR_BIND(CUnitScriptEngine, )

CR_REG_METADATA(CUnitScriptEngine, (
	CR_MEMBER(animating),

	// always null when saving
	CR_IGNORED(currentScript)
))

static bool ReplayCheckpointDebugUnitScriptFrame()
{
	return (gs != nullptr && configHandler != nullptr && gs->frameNum == configHandler->GetInt("ReplayCheckpointDebugSignatureFrame"));
}

static bool ReplayCheckpointShouldLogUnitScript(const CUnitScript* script)
{
	const CUnit* unit = (script != nullptr) ? script->GetUnit() : nullptr;

	if (unit == nullptr)
		return false;

	const int debugUnitID = configHandler->GetInt("ReplayCheckpointDebugCobUnitID");
	return (debugUnitID == -1 || unit->id == debugUnitID);
}

static void LogReplayCheckpointAnimatingState(const char* phase, const std::vector<CUnitScript*>& animating, uint32_t cs)
{
	if (!ReplayCheckpointDebugUnitScriptFrame())
		return;

	LOG("[ReplayCheckpoint][unit-script-sig] %s frame=%d cs=%08x count=%u",
		phase,
		gs->frameNum,
		cs,
		static_cast<unsigned int>(animating.size())
	);

	for (size_t i = 0; i < animating.size(); ++i) {
		const CUnitScript* script = animating[i];
		if (!ReplayCheckpointShouldLogUnitScript(script))
			continue;

		const CUnit* unit = (script != nullptr) ? script->GetUnit() : nullptr;
		const auto liveCount = (script != nullptr) ? script->GetLiveAnims().size() : 0;
		const auto doneCount = (script != nullptr) ? script->GetDoneAnims().size() : 0;

		LOG("[ReplayCheckpoint][unit-script-sig] %s frame=%d index=%u unit=%d checksum=%08x live=%u liveHash=%08x done=%u doneHash=%08x",
			phase,
			gs->frameNum,
			static_cast<unsigned int>(i),
			(unit != nullptr) ? unit->id : -1,
			(script != nullptr) ? script->GetAnimArrayChecksum() : 0u,
			static_cast<unsigned int>(liveCount),
			(script != nullptr) ? script->GetLiveAnimHash() : 0u,
			static_cast<unsigned int>(doneCount),
			(script != nullptr) ? script->GetDoneAnimHash() : 0u
		);
	}
}


void CUnitScriptEngine::InitStatic() {
	RECOIL_DETAILED_TRACY_ZONE;
	cobEngine = &gCobEngine;
	cobFileHandler = &gCobFileHandler;
	unitScriptEngine = &gUnitScriptEngine;

	cobEngine->Init();
	cobFileHandler->Init();
	unitScriptEngine->Init();
}

void CUnitScriptEngine::KillStatic() {
	RECOIL_DETAILED_TRACY_ZONE;
	cobEngine->Kill();
	cobFileHandler->Kill();
	unitScriptEngine->Kill();

	cobEngine = nullptr;
	cobFileHandler = nullptr;
	unitScriptEngine = nullptr;
}



void CUnitScriptEngine::ReloadScripts(const UnitDef* udef)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const CCobFile* oldScriptFile = cobFileHandler->GetScriptFile(udef->scriptName);

	if (oldScriptFile == nullptr) {
		LOG_L(L_WARNING, "[UnitScriptEngine::%s] unknown COB script for unit \"%s\": %s", __func__, udef->name.c_str(), udef->scriptName.c_str());
		return;
	}

	CCobFile* newScriptFile = cobFileHandler->ReloadCobFile(udef->scriptName);

	if (newScriptFile == nullptr) {
		LOG_L(L_WARNING, "[UnitScriptEngine::%s] could not load COB script for unit \"%s\" from: %s", __func__, udef->name.c_str(), udef->scriptName.c_str());
		return;
	}

	unsigned int count = 0;

	for (unsigned int i = 0, n = unitHandler.MaxUnits(); i < n; i++) {
		CUnit* unit = unitHandler.GetUnit(i);

		if (unit == nullptr)
			continue;

		CUnitScript*& unitScript = unit->script;
		CCobInstance* cobInstance = dynamic_cast<CCobInstance*>(unitScript);

		if (cobInstance == nullptr || cobInstance->GetFile() != oldScriptFile)
			continue;

		count++;

		spring::SafeDestruct(unitScript);

		unitScript = CUnitScriptFactory::CreateCOBScript(unit, newScriptFile);
		unitScript->Create();
	}

	LOG("[UnitScriptEngine::%s] reloaded COB scripts for %i units", __func__, count);
}


void CUnitScriptEngine::AddInstance(CUnitScript* instance)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (instance == currentScript)
		return;

	spring::VectorInsertUnique(animating, instance/*, true*/);
}

void CUnitScriptEngine::RemoveInstance(CUnitScript* instance)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (instance == currentScript)
		return;

	spring::VectorErase(animating, instance);
}

void CUnitScriptEngine::Tick(int deltaTime)
{
	SCOPED_TIMER("CUnitScriptEngine::Tick");

	cobEngine->Tick(deltaTime);
	LogReplayCheckpointAnimatingState("tick-begin", animating, 0);

	// tick all (COB or LUS) script instances that have registered themselves as animating
	{
		ZoneScopedN("CUnitScriptEngine::Tick(MT)");

		// setting currentScript = animating[i]; is not required here, only in ST section below
		for_mt(0, animating.size(), [&](const int i) {
#ifdef SYNCCHECK
			const CUnitScript* debugScript = animating[i];
			const bool replayCheckpointDebug = ReplayCheckpointDebugUnitScriptFrame() && ReplayCheckpointShouldLogUnitScript(debugScript);
			if (replayCheckpointDebug) {
				const CUnit* unit = debugScript->GetUnit();
				LOG("[ReplayCheckpoint][unit-script-tick] before frame=%d index=%d unit=%d sync=%08x live=%u liveHash=%08x",
					gs->frameNum,
					i,
					(unit != nullptr) ? unit->id : -1,
					CSyncChecker::GetChecksum(),
					static_cast<unsigned int>(debugScript->GetLiveAnims().size()),
					debugScript->GetLiveAnimHash()
				);
			}
#endif
			animating[i]->TickAllAnims(deltaTime);
#ifdef SYNCCHECK
			if (replayCheckpointDebug) {
				const CUnitScript* script = animating[i];
				const CUnit* unit = (script != nullptr) ? script->GetUnit() : nullptr;
				LOG("[ReplayCheckpoint][unit-script-tick] after frame=%d index=%d unit=%d sync=%08x live=%u liveHash=%08x done=%u doneHash=%08x",
					gs->frameNum,
					i,
					(unit != nullptr) ? unit->id : -1,
					CSyncChecker::GetChecksum(),
					(script != nullptr) ? static_cast<unsigned int>(script->GetLiveAnims().size()) : 0u,
					(script != nullptr) ? script->GetLiveAnimHash() : 0u,
					(script != nullptr) ? static_cast<unsigned int>(script->GetDoneAnims().size()) : 0u,
					(script != nullptr) ? script->GetDoneAnimHash() : 0u
				);
			}
#endif
		});
	}
	LogReplayCheckpointAnimatingState("after-tick-all-anims", animating, 0);
	{
		ZoneScopedN("CUnitScriptEngine::Tick(ST)");

		uint32_t cs = 0;
		for (size_t i = 0; i < animating.size(); /*NO-OP*/) {
			currentScript = animating[i];
			// deal with synced checksum here, before animating is possibly popped below
			const uint32_t csBefore = cs;
			const uint32_t scriptChecksum = currentScript->GetAnimArrayChecksum();
			cs = spring::hash_combine(currentScript->GetAnimArrayChecksum(), cs);
			const bool replayCheckpointDebug = ReplayCheckpointDebugUnitScriptFrame() && ReplayCheckpointShouldLogUnitScript(currentScript);

			if (!currentScript->TickAnimFinished()) {
				if (replayCheckpointDebug) {
					const CUnit* unit = currentScript->GetUnit();
					LOG("[ReplayCheckpoint][unit-script-sig] st-step frame=%d index=%u unit=%d checksum=%08x csBefore=%08x csAfter=%08x live=%u done=%u keep=0",
						gs->frameNum,
						static_cast<unsigned int>(i),
						(unit != nullptr) ? unit->id : -1,
						scriptChecksum,
						csBefore,
						cs,
						static_cast<unsigned int>(currentScript->GetLiveAnims().size()),
						static_cast<unsigned int>(currentScript->GetDoneAnims().size())
					);
				}
				animating[i] = animating.back();
				animating.pop_back();
				continue;
			}
			if (replayCheckpointDebug) {
				const CUnit* unit = currentScript->GetUnit();
				LOG("[ReplayCheckpoint][unit-script-sig] st-step frame=%d index=%u unit=%d checksum=%08x csBefore=%08x csAfter=%08x live=%u done=%u keep=1",
					gs->frameNum,
					static_cast<unsigned int>(i),
					(unit != nullptr) ? unit->id : -1,
					scriptChecksum,
					csBefore,
					cs,
					static_cast<unsigned int>(currentScript->GetLiveAnims().size()),
					static_cast<unsigned int>(currentScript->GetDoneAnims().size())
				);
			}
			i++;
		}

		currentScript = nullptr;
		LogReplayCheckpointAnimatingState("before-sync", animating, cs);
		Sync::Assert(cs, "animating");
	}

	cobEngine->RunDeferredCallins();
}
