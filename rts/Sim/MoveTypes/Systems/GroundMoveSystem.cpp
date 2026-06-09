/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// #undef NDEBUG

#include "GroundMoveSystem.h"

#include "Sim/Ecs/Registry.h"
#include "Sim/Features/Feature.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/MoveTypes/Components/MoveTypesComponents.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"

#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"
#ifdef SYNCCHECK
	#include "System/Sync/SyncChecker.h"
#endif

#include <algorithm>
#include <limits>
#include <vector>

using namespace MoveTypes;

void GroundMoveSystem::Init() {}

namespace {
	int GetMoveEntityUnitId(const entt::entity entity)
	{
		if (const auto* groundMoveType = Sim::registry.try_get<GroundMoveType>(entity); groundMoveType != nullptr)
			return groundMoveType->value;

		if (const auto* generalMoveType = Sim::registry.try_get<GeneralMoveType>(entity); generalMoveType != nullptr)
			return generalMoveType->value;

		if (const auto* headingEvent = Sim::registry.try_get<ChangeHeadingEvent>(entity); headingEvent != nullptr)
			return headingEvent->unitId;

		if (const auto* mainHeadingEvent = Sim::registry.try_get<ChangeMainHeadingEvent>(entity); mainHeadingEvent != nullptr)
			return mainHeadingEvent->unitId;

		return std::numeric_limits<int>::max();
	}

	template<typename T>
	std::vector<entt::entity> GetOrderedComponentEntities()
	{
		auto view = Sim::registry.view<T>();

		std::vector<entt::entity> entities;
		entities.reserve(view.size());

		for (const entt::entity entity: view) {
			entities.push_back(entity);
		}

		std::sort(entities.begin(), entities.end(), [](const entt::entity lhs, const entt::entity rhs) {
			const int lhsUnitId = GetMoveEntityUnitId(lhs);
			const int rhsUnitId = GetMoveEntityUnitId(rhs);

			if (lhsUnitId != rhsUnitId)
				return lhsUnitId < rhsUnitId;

			return entt::to_integral(lhs) < entt::to_integral(rhs);
		});

		return entities;
	}

	template<typename T>
	std::vector<int> GetOrderedMoveUnitIds()
	{
		auto view = Sim::registry.view<T>();

		std::vector<int> unitIds;
		unitIds.reserve(view.size());

		for (const entt::entity entity: view) {
			unitIds.push_back(view.template get<T>(entity).value);
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

	CGroundMoveType* GetGroundMoveType(CUnit* unit)
	{
		if (unit == nullptr)
			return nullptr;

		CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
		assert(moveType != nullptr);
		return moveType;
	}

#ifdef SYNCCHECK
	bool ReplayCheckpointDebugGroundMoveSystemFrame()
	{
		return (
			gs != nullptr &&
			configHandler != nullptr &&
			gs->frameNum == configHandler->GetInt("ReplayCheckpointDebugSignatureFrame")
		);
	}

	void LogReplayCheckpointGroundMoveBoundary(const char* phase, const int index, const CUnit* unit, const CGroundMoveType* moveType)
	{
		if (!ReplayCheckpointDebugGroundMoveSystemFrame())
			return;
		if (unit == nullptr || moveType == nullptr)
			return;

		const auto& currWayPoint = moveType->GetCurrWayPoint();
		const auto& nextWayPoint = moveType->GetNextWayPoint();
		const float3& earlyCurrWayPoint = moveType->GetEarlyCurrWayPoint();
		const float3& earlyNextWayPoint = moveType->GetEarlyNextWayPoint();

		LOG("[ReplayCheckpoint][ground-move-boundary] phase=%s frame=%d index=%d unit=%d sync=%08x pos=<%f,%f,%f> speed=<%f,%f,%f,%f> heading=%d cwp=<%f,%f,%f> nwp=<%f,%f,%f> ecwp=<%f,%f,%f> enwp=<%f,%f,%f> path=%u nextPath=%u cwpDist=%f prevCwpDist=%f atGoal=%d atEnd=%d pathingArrived=%d pathingFailed=%d",
			phase,
			gs->frameNum,
			index,
			unit->id,
			CSyncChecker::GetChecksum(),
			float(unit->pos.x), float(unit->pos.y), float(unit->pos.z),
			float(unit->speed.x), float(unit->speed.y), float(unit->speed.z), float(unit->speed.w),
			int(unit->heading),
			float(currWayPoint.x), float(currWayPoint.y), float(currWayPoint.z),
			float(nextWayPoint.x), float(nextWayPoint.y), float(nextWayPoint.z),
			earlyCurrWayPoint.x, earlyCurrWayPoint.y, earlyCurrWayPoint.z,
			earlyNextWayPoint.x, earlyNextWayPoint.y, earlyNextWayPoint.z,
			moveType->GetPathID(),
			moveType->GetNextPathID(),
			moveType->GetCurrWayPointDist(),
			moveType->GetPrevWayPointDist(),
			int(moveType->IsAtGoal()),
			int(moveType->IsAtEndOfPath()),
			int(moveType->IsPathingArrived()),
			int(moveType->IsPathingFailed())
		);
	}
#else
	void LogReplayCheckpointGroundMoveBoundary(const char*, const int, const CUnit*, const CGroundMoveType*) {}
#endif
}

template<typename T, typename F>
void issue_events(F func)
{
    for (const entt::entity entity: GetOrderedComponentEntities<T>()) {
		if (!Sim::registry.valid(entity) || !Sim::registry.all_of<T>(entity))
			continue;

		T& comp = Sim::registry.get<T>(entity);
        std::for_each(comp.value.begin(), comp.value.end(), func);
        comp.value.clear();
    }
}

void GroundMoveSystem::Update() {
    // TODO: GroundMove could become a component (or series of components) and then the extra indirection wouldn't be
    // needed. Though that will be a bigger change.
	{
		SCOPED_TIMER("Sim::Unit::MoveType::1::UpdateTraversalPlan");
        const std::vector<int> unitIds = GetOrderedMoveUnitIds<GroundMoveType>();
        for_mt(0, static_cast<int>(unitIds.size()), [&unitIds](const int i){
            CUnit* unit = GetMoveUnit(unitIds[i]);
			CGroundMoveType* moveType = GetGroundMoveType(unit);

			if (moveType == nullptr)
				return;

            #ifndef NDEBUG
			unit->SanityCheck();
            #endif

			LogReplayCheckpointGroundMoveBoundary("traversal-before", i, unit, moveType);
			moveType->UpdateTraversalPlan();
			LogReplayCheckpointGroundMoveBoundary("traversal-after", i, unit, moveType);
		});
	}
	{
		SCOPED_TIMER("Sim::Unit::MoveType::2::UpdatePreCollisions");

        // These two sections are ST due to the numerous synced vars being changed.
        {
            for (const entt::entity entity: GetOrderedComponentEntities<ChangeHeadingEvent>()) {
				if (!Sim::registry.valid(entity) || !Sim::registry.all_of<ChangeHeadingEvent>(entity))
					continue;

				ChangeHeadingEvent& event = Sim::registry.get<ChangeHeadingEvent>(entity);
                if (event.changed) {
                    CUnit* unit = GetMoveUnit(event.unitId);
                    CGroundMoveType* moveType = GetGroundMoveType(unit);
					if (moveType != nullptr)
						moveType->ChangeHeading(event.deltaHeading);

                    event.changed = false;
                }
            }
        }
        {
            for (const entt::entity entity: GetOrderedComponentEntities<ChangeMainHeadingEvent>()) {
				if (!Sim::registry.valid(entity) || !Sim::registry.all_of<ChangeMainHeadingEvent>(entity))
					continue;

				ChangeMainHeadingEvent& event = Sim::registry.get<ChangeMainHeadingEvent>(entity);
                if (event.changed) {
                    CUnit* unit = GetMoveUnit(event.unitId);
                    CGroundMoveType* moveType = GetGroundMoveType(unit);
					if (moveType != nullptr)
						moveType->SetMainHeading();

                    event.changed = false;
                }
            }
        }
    }
	{
        const std::vector<int> unitIds = GetOrderedMoveUnitIds<GroundMoveType>();
        for_mt(0, static_cast<int>(unitIds.size()), [&unitIds](const int i){
            CUnit* unit = GetMoveUnit(unitIds[i]);
			CGroundMoveType* moveType = GetGroundMoveType(unit);

			if (moveType == nullptr)
				return;

			LogReplayCheckpointGroundMoveBoundary("unit-position-before", i, unit, moveType);
			moveType->UpdateUnitPosition();
			LogReplayCheckpointGroundMoveBoundary("unit-position-after", i, unit, moveType);
		});

		int index = 0;
		for (const int unitId: GetOrderedMoveUnitIds<GroundMoveType>()) {
			CUnit* unit = GetMoveUnit(unitId);
			CGroundMoveType* moveType = GetGroundMoveType(unit);
			if (moveType == nullptr)
				continue;

			LogReplayCheckpointGroundMoveBoundary("pre-collisions-before", index, unit, moveType);
			moveType->UpdatePreCollisions();
			LogReplayCheckpointGroundMoveBoundary("pre-collisions-after", index, unit, moveType);

            // this unit is not coming back, kill it now without any death
            // sequence (s.t. deathScriptFinished becomes true immediately)
            if (!unit->pos.IsInBounds() && (unit->speed.w > MAX_UNIT_SPEED))
                unit->ForcedKillUnit(nullptr, false, true, -CSolidObject::DAMAGE_KILLED_OOB);

			++index;
		}
	}
    {
        SCOPED_TIMER("Sim::Unit::MoveType::3::CollisionDetection");
        const std::vector<int> unitIds = GetOrderedMoveUnitIds<GroundMoveType>();
        for_mt(0, static_cast<int>(unitIds.size()), [&unitIds](const int i){
            CUnit* unit = GetMoveUnit(unitIds[i]);
            if (unit == nullptr)
                return;

            assert( Sim::registry.valid(unit->entityReference) );
            assert( Sim::registry.all_of<GroundMoveType>(unit->entityReference) );
            assert( !Sim::registry.all_of<GeneralMoveType>(unit->entityReference) );

            CGroundMoveType* moveType = GetGroundMoveType(unit);
            if (moveType == nullptr)
                return;

            moveType->SetMtJobId(i);
			LogReplayCheckpointGroundMoveBoundary("collision-detection-before", i, unit, moveType);
            moveType->UpdateCollisionDetections();
			LogReplayCheckpointGroundMoveBoundary("collision-detection-after", i, unit, moveType);
        });
    }
	{
        SCOPED_TIMER("Sim::Unit::MoveType::4::ProcessCollisionEvents");

        issue_events<UnitCrushEvents>([](const UnitCrushEvent& event) {
            event.collidee->Kill(event.collider, event.crushImpulse, true);
        });
        issue_events<FeatureCrushEvents>([](const FeatureCrushEvent& event) {
            event.collidee->Kill(event.collider, event.crushImpulse, true);
        });
        issue_events<UnitCollisionEvents>([&](const UnitCollisionEvent& event) {
            eventHandler.UnitUnitCollision(event.collider, event.collidee);
        });
        issue_events<FeatureCollisionEvents>([](const FeatureCollisionEvent& event) {
            eventHandler.UnitFeatureCollision(event.collider, event.collidee);
        });
        issue_events<FeatureMoveEvents>([](const FeatureMoveEvent& event) {
            quadField.RemoveFeature(event.collidee);
            event.collidee->Move(event.moveImpulse, true);
            quadField.AddFeature(event.collidee);
        });
	}
	{
        // TODO: the vars are synced and that's what is stopping this being MT'ed.
        // Need an alternative method to support sync values that doesn't stop MT.
        // Same for change heading above as well.
        SCOPED_TIMER("Sim::Unit::MoveType::5::Update");
        for (const int unitId: GetOrderedMoveUnitIds<GroundMoveType>()) {
            CUnit* unit = GetMoveUnit(unitId);
            CGroundMoveType* moveType = GetGroundMoveType(unit);
            if (moveType == nullptr)
                continue;

			LogReplayCheckpointGroundMoveBoundary("update-before", unitId, unit, moveType);
            if (moveType->Update()) 
                eventHandler.UnitMoved(unit);
			LogReplayCheckpointGroundMoveBoundary("update-after", unitId, unit, moveType);

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif
        }
    }
}

void GroundMoveSystem::Shutdown() {}
