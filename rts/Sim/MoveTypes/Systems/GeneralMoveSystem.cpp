/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// #undef NDEBUG

#include "GeneralMoveSystem.h"

#include "Sim/Ecs/Registry.h"
#include "Sim/MoveTypes/Components/MoveTypesComponents.h"
#include "Sim/MoveTypes/AAirMoveType.h"
#include "Sim/MoveTypes/HoverAirMoveType.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/MoveTypes/StrafeAirMoveType.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"

#include "System/EventHandler.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/TimeProfiler.h"
#include "System/Sync/SyncChecker.h"
#include "System/Threading/ThreadPool.h"
#include "Sim/Units/UnitDef.h"

#include "System/Misc/TracyDefs.h"

#include <algorithm>
#include <cassert>
#include <vector>

using namespace MoveTypes;

namespace {
	std::vector<int> GetOrderedMoveUnitIds()
	{
		auto view = Sim::registry.view<GeneralMoveType>();

		std::vector<int> unitIds;
		unitIds.reserve(view.size());

		for (const entt::entity entity: view) {
			unitIds.push_back(view.get<GeneralMoveType>(entity).value);
		}

		std::sort(unitIds.begin(), unitIds.end());
		return unitIds;
	}

	CUnit* GetMoveUnit(const int unitId)
	{
		CUnit* unit = unitHandler.GetUnit(unitId);
		assert(unit != nullptr);
		return unit;
	}

	bool ReplayCheckpointDebugGeneralMoveFrame()
	{
		return (
			configHandler != nullptr &&
			gs != nullptr &&
			gs->frameNum == configHandler->GetInt("ReplayCheckpointDebugSignatureFrame")
		);
	}

	void LogReplayCheckpointGeneralMoveBoundary(
		const char* phase,
		const int index,
		const int unitId,
		const CUnit* unit,
		const AMoveType* moveType
	) {
		if (!ReplayCheckpointDebugGeneralMoveFrame() || unit == nullptr)
			return;

		const float3& goalPos = (moveType != nullptr)? moveType->goalPos: ZeroVector;
		const float3& oldPos = (moveType != nullptr)? moveType->oldPos: ZeroVector;
		const float3& oldSlowUpdatePos = (moveType != nullptr)? moveType->oldSlowUpdatePos: ZeroVector;
		const float3& oldCollisionUpdatePos = (moveType != nullptr)? moveType->oldCollisionUpdatePos: ZeroVector;

		LOG("[ReplayCheckpoint][general-move-boundary] phase=%s frame=%d index=%d unit=%d lookup=%d sync=%08x pos=<%f,%f,%f> speed=<%f,%f,%f,%f> heading=%d front=<%f,%f,%f> right=<%f,%f,%f> up=<%f,%f,%f> mid=<%f,%f,%f> aim=<%f,%f,%f> goal=<%f,%f,%f> old=<%f,%f,%f> oldSlow=<%f,%f,%f> oldColl=<%f,%f,%f> progress=%d useHeading=%u",
			phase,
			gs->frameNum,
			index,
			unit->id,
			unitId,
			CSyncChecker::GetChecksum(),
			unit->pos.x, unit->pos.y, unit->pos.z,
			unit->speed.x, unit->speed.y, unit->speed.z, unit->speed.w,
			static_cast<int>(unit->heading),
			static_cast<float>(unit->frontdir.x), static_cast<float>(unit->frontdir.y), static_cast<float>(unit->frontdir.z),
			static_cast<float>(unit->rightdir.x), static_cast<float>(unit->rightdir.y), static_cast<float>(unit->rightdir.z),
			static_cast<float>(unit->updir.x), static_cast<float>(unit->updir.y), static_cast<float>(unit->updir.z),
			static_cast<float>(unit->midPos.x), static_cast<float>(unit->midPos.y), static_cast<float>(unit->midPos.z),
			static_cast<float>(unit->aimPos.x), static_cast<float>(unit->aimPos.y), static_cast<float>(unit->aimPos.z),
			goalPos.x, goalPos.y, goalPos.z,
			oldPos.x, oldPos.y, oldPos.z,
			oldSlowUpdatePos.x, oldSlowUpdatePos.y, oldSlowUpdatePos.z,
			oldCollisionUpdatePos.x, oldCollisionUpdatePos.y, oldCollisionUpdatePos.z,
			(moveType != nullptr)? static_cast<int>(moveType->progressState): -1,
			(moveType != nullptr && moveType->UseHeading())? 1u: 0u
		);
	}

	void LogReplayCheckpointAirMoveBoundary(
		const char* phase,
		const int index,
		const int unitId,
		const CUnit* unit,
		const AMoveType* moveType
	) {
		if (!ReplayCheckpointDebugGeneralMoveFrame() || unit == nullptr || moveType == nullptr)
			return;

		const AAirMoveType* airMoveType = dynamic_cast<const AAirMoveType*>(moveType);
		if (airMoveType == nullptr)
			return;

		const CStrafeAirMoveType* strafeMoveType = dynamic_cast<const CStrafeAirMoveType*>(moveType);
		const CHoverAirMoveType* hoverMoveType = dynamic_cast<const CHoverAirMoveType*>(moveType);
		const char* moveTypeName = (strafeMoveType != nullptr)? "strafe": ((hoverMoveType != nullptr)? "hover": "air");
		const char* unitDefName = (unit->unitDef != nullptr)? unit->unitDef->name.c_str(): "";

		LOG("[ReplayCheckpoint][air-move-boundary] phase=%s frame=%d index=%d unit=%d lookup=%d def=%s kind=%s sync=%08x aircraft=%d collision=%d oldGoal=<%.9g,%.9g,%.9g> reserved=<%.9g,%.9g,%.9g> landSq=%.9g wantedH=%.9g orgWantedH=%.9g acc=%.9g dec=%.9g alt=%.9g collide=%u autoLand=%u dontLand=%u smooth=%u submerge=%u floatWater=%u strafeBlock=%d strafeState=%d strafeSub=%d loopback=%u fighter=%u wingDrag=%.9g wingAngle=%.9g invDrag=%.9g crashDrag=%.9g frontToSpeed=%.9g speedToFront=%.9g myGravity=%.9g maxBank=%.9g maxPitch=%.9g turnRadius=%.9g maxAileron=%.9g maxElevator=%.9g maxRudder=%.9g attackSafety=%.9g crashCtrl=<%.9g,%.9g,%.9g> lastRudder=<%.9g,%.9g> lastElevator=<%.9g,%.9g> lastAileron=<%.9g,%.9g> hoverFly=%d hoverBanking=%u hoverStrafe=%u hoverStop=%u hoverGoalDist=%.9g hoverBank=%.9g hoverPitch=%.9g hoverTurn=%.9g hoverMaxDrift=%.9g hoverMaxTurn=%.9g hoverWantedHeading=%d hoverForcedHeading=%d hoverAllowLand=%u",
			phase,
			gs->frameNum,
			index,
			unit->id,
			unitId,
			unitDefName,
			moveTypeName,
			CSyncChecker::GetChecksum(),
			static_cast<int>(airMoveType->aircraftState),
			static_cast<int>(airMoveType->collisionState),
			airMoveType->oldGoalPos.x, airMoveType->oldGoalPos.y, airMoveType->oldGoalPos.z,
			airMoveType->reservedLandingPos.x, airMoveType->reservedLandingPos.y, airMoveType->reservedLandingPos.z,
			airMoveType->landRadiusSq,
			airMoveType->wantedHeight,
			airMoveType->orgWantedHeight,
			airMoveType->accRate,
			airMoveType->decRate,
			airMoveType->altitudeRate,
			airMoveType->collide? 1u: 0u,
			airMoveType->autoLand? 1u: 0u,
			airMoveType->dontLand? 1u: 0u,
			airMoveType->useSmoothMesh? 1u: 0u,
			airMoveType->canSubmerge? 1u: 0u,
			airMoveType->floatOnWater? 1u: 0u,
			(strafeMoveType != nullptr)? strafeMoveType->maneuverBlockTime: 0,
			(strafeMoveType != nullptr)? strafeMoveType->maneuverState: 0,
			(strafeMoveType != nullptr)? strafeMoveType->maneuverSubState: 0,
			(strafeMoveType != nullptr && strafeMoveType->loopbackAttack)? 1u: 0u,
			(strafeMoveType != nullptr && strafeMoveType->isFighter)? 1u: 0u,
			(strafeMoveType != nullptr)? strafeMoveType->wingDrag: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->wingAngle: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->invDrag: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->crashDrag: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->frontToSpeed: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->speedToFront: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->myGravity: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->maxBank: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->maxPitch: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->turnRadius: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->maxAileron: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->maxElevator: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->maxRudder: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->attackSafetyDistance: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->crashRudder: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->crashElevator: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->crashAileron: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->lastRudderPos[0]: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->lastRudderPos[1]: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->lastElevatorPos[0]: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->lastElevatorPos[1]: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->lastAileronPos[0]: 0.0f,
			(strafeMoveType != nullptr)? strafeMoveType->lastAileronPos[1]: 0.0f,
			(hoverMoveType != nullptr)? static_cast<int>(hoverMoveType->flyState): 0,
			(hoverMoveType != nullptr && hoverMoveType->bankingAllowed)? 1u: 0u,
			(hoverMoveType != nullptr && hoverMoveType->airStrafe)? 1u: 0u,
			(hoverMoveType != nullptr && hoverMoveType->wantToStop)? 1u: 0u,
			(hoverMoveType != nullptr)? hoverMoveType->goalDistance: 0.0f,
			(hoverMoveType != nullptr)? hoverMoveType->currentBank: 0.0f,
			(hoverMoveType != nullptr)? hoverMoveType->currentPitch: 0.0f,
			(hoverMoveType != nullptr)? hoverMoveType->turnRate: 0.0f,
			(hoverMoveType != nullptr)? hoverMoveType->maxDrift: 0.0f,
			(hoverMoveType != nullptr)? hoverMoveType->maxTurnAngle: 0.0f,
			(hoverMoveType != nullptr)? static_cast<int>(hoverMoveType->GetWantedHeading()): 0,
			(hoverMoveType != nullptr)? static_cast<int>(hoverMoveType->GetForcedHeading()): 0,
			(hoverMoveType != nullptr && hoverMoveType->GetAllowLanding())? 1u: 0u
		);
	}
}

void GeneralMoveSystem::Init() {
    RECOIL_DETAILED_TRACY_ZONE;
    CMoveMath::InitRangeIsBlockedHashes();
    Sim::systemUtils.OnPostLoad().connect<&CMoveMath::InitRangeIsBlockedHashes>();
}

void GeneralMoveSystem::Update() {
    RECOIL_DETAILED_TRACY_ZONE;
	{
        SCOPED_TIMER("Sim::Unit::MoveType::5::Update");
		const std::vector<int> unitIds = GetOrderedMoveUnitIds();
        for (size_t i = 0; i < unitIds.size(); ++i) {
			const int unitId = unitIds[i];
            CUnit* unit = GetMoveUnit(unitId);
			if (unit == nullptr)
				continue;

            AMoveType* moveType = unit->moveType;
			LogReplayCheckpointGeneralMoveBoundary("before", static_cast<int>(i), unitId, unit, moveType);
			LogReplayCheckpointAirMoveBoundary("before", static_cast<int>(i), unitId, unit, moveType);

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif

			const bool moved = moveType->Update();
			LogReplayCheckpointGeneralMoveBoundary("after-update", static_cast<int>(i), unitId, unit, moveType);
			LogReplayCheckpointAirMoveBoundary("after-update", static_cast<int>(i), unitId, unit, moveType);

            if (moved)
                eventHandler.UnitMoved(unit);
			LogReplayCheckpointGeneralMoveBoundary("after-event", static_cast<int>(i), unitId, unit, moveType);
			LogReplayCheckpointAirMoveBoundary("after-event", static_cast<int>(i), unitId, unit, moveType);

            // this unit is not coming back, kill it now without any death
            // sequence (s.t. deathScriptFinished becomes true immediately)
            if (!unit->pos.IsInBounds() && (unit->speed.w > MAX_UNIT_SPEED))
                unit->ForcedKillUnit(nullptr, false, true, -CSolidObject::DAMAGE_KILLED_OOB);
			LogReplayCheckpointGeneralMoveBoundary("after-oob", static_cast<int>(i), unitId, unit, moveType);
			LogReplayCheckpointAirMoveBoundary("after-oob", static_cast<int>(i), unitId, unit, moveType);

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif
        }
	}
}

void GeneralMoveSystem::Shutdown() {
    Sim::systemUtils.OnPostLoad().disconnect<&CMoveMath::InitRangeIsBlockedHashes>();
}
