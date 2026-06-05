/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cassert>
#include <cstring>

#include "UnitHandler.h"
#include "Unit.h"
#include "UnitDefHandler.h"
#include "UnitMemPool.h"
#include "UnitTypes/Builder.h"
#include "UnitTypes/ExtractorBuilding.h"
#include "UnitTypes/Factory.h"

#include "CommandAI/BuilderCAI.h"
#include "Sim/Ecs/Registry.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/GroundMoveType.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/MoveTypes/Systems/GeneralMoveSystem.h"
#include "Sim/MoveTypes/Systems/GroundMoveSystem.h"
#include "Sim/MoveTypes/Systems/UnitTrapCheckSystem.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Weapons/Weapon.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
#include "System/SpringMath.h"
#include "System/SpringHash.h"
#include "System/Sync/SyncChecker.h"
#include "System/Threading/ThreadPool.h"
#include "System/TimeProfiler.h"
#include "System/creg/STL_Deque.h"
#include "System/creg/STL_Set.h"
#include "Sim/Path/HAPFS/PathGlobal.h"

#include "System/Misc/TracyDefs.h"

#include "System/Config/ConfigHandler.h"
CONFIG(bool, UpdateWeaponVectorsMT).deprecated(true);
CONFIG(bool, UpdateBoundingVolumeMT).deprecated(true);

static uint32_t ReplayCheckpointUnitHandlerHashInt(uint32_t hash, int value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointUnitHandlerHashUInt(uint32_t hash, uint32_t value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointUnitHandlerHashBool(uint32_t hash, bool value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointUnitHandlerHashFloat(uint32_t hash, float value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointUnitHandlerHashFloat3(uint32_t hash, const float3& value)
{
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.x);
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.y);
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.z);
	return hash;
}

static uint32_t ReplayCheckpointUnitHandlerHashFloat4(uint32_t hash, const float4& value)
{
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.x);
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.y);
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.z);
	hash = ReplayCheckpointUnitHandlerHashFloat(hash, value.w);
	return hash;
}

static uint32_t ReplayCheckpointUnitHandlerHashUnitMoveState(const CUnit* unit)
{
	uint32_t hash = 0x31415926u;

	hash = ReplayCheckpointUnitHandlerHashInt(hash, unit->id);
	hash = ReplayCheckpointUnitHandlerHashInt(hash, unit->team);
	hash = ReplayCheckpointUnitHandlerHashFloat3(hash, unit->pos);
	hash = ReplayCheckpointUnitHandlerHashFloat4(hash, unit->speed);
	hash = ReplayCheckpointUnitHandlerHashInt(hash, unit->heading);
	hash = ReplayCheckpointUnitHandlerHashUInt(hash, static_cast<uint32_t>(unit->physicalState));
	hash = ReplayCheckpointUnitHandlerHashUInt(hash, static_cast<uint32_t>(unit->collidableState));
	hash = ReplayCheckpointUnitHandlerHashFloat3(hash, unit->frontdir);
	hash = ReplayCheckpointUnitHandlerHashFloat3(hash, unit->rightdir);
	hash = ReplayCheckpointUnitHandlerHashFloat3(hash, unit->updir);
	hash = ReplayCheckpointUnitHandlerHashFloat3(hash, unit->midPos);
	hash = ReplayCheckpointUnitHandlerHashFloat3(hash, unit->aimPos);

	const AMoveType* moveType = unit->moveType;
	hash = ReplayCheckpointUnitHandlerHashUInt(hash, static_cast<uint32_t>(moveType != nullptr));
	if (moveType != nullptr) {
		hash = ReplayCheckpointUnitHandlerHashFloat3(hash, moveType->goalPos);
		hash = ReplayCheckpointUnitHandlerHashFloat3(hash, moveType->oldPos);
		hash = ReplayCheckpointUnitHandlerHashFloat3(hash, moveType->oldSlowUpdatePos);
		hash = ReplayCheckpointUnitHandlerHashFloat3(hash, moveType->oldCollisionUpdatePos);
		hash = ReplayCheckpointUnitHandlerHashInt(hash, static_cast<int>(moveType->progressState));
		hash = ReplayCheckpointUnitHandlerHashBool(hash, moveType->UseHeading());

		const CGroundMoveType* groundMoveType = dynamic_cast<const CGroundMoveType*>(moveType);
		hash = ReplayCheckpointUnitHandlerHashUInt(hash, static_cast<uint32_t>(groundMoveType != nullptr));
		if (groundMoveType != nullptr) {
			hash = ReplayCheckpointUnitHandlerHashFloat3(hash, groundMoveType->GetCurrWayPoint());
			hash = ReplayCheckpointUnitHandlerHashFloat3(hash, groundMoveType->GetNextWayPoint());
			hash = ReplayCheckpointUnitHandlerHashFloat3(hash, groundMoveType->GetEarlyCurrWayPoint());
			hash = ReplayCheckpointUnitHandlerHashFloat3(hash, groundMoveType->GetEarlyNextWayPoint());
			hash = ReplayCheckpointUnitHandlerHashFloat3(hash, groundMoveType->GetWaypointDir());
			hash = ReplayCheckpointUnitHandlerHashFloat3(hash, groundMoveType->GetFlatFrontDir());
			hash = ReplayCheckpointUnitHandlerHashFloat(hash, groundMoveType->GetWantedSpeed());
			hash = ReplayCheckpointUnitHandlerHashFloat(hash, groundMoveType->GetCurrentSpeed());
			hash = ReplayCheckpointUnitHandlerHashFloat(hash, groundMoveType->GetDeltaSpeed());
			hash = ReplayCheckpointUnitHandlerHashFloat(hash, groundMoveType->GetCurrWayPointDist());
			hash = ReplayCheckpointUnitHandlerHashFloat(hash, groundMoveType->GetPrevWayPointDist());
			hash = ReplayCheckpointUnitHandlerHashUInt(hash, groundMoveType->GetPathID());
			hash = ReplayCheckpointUnitHandlerHashUInt(hash, groundMoveType->GetNextPathID());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsReversing());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsAtGoal());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsAtEndOfPath());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsLastWaypoint());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsUsingRawMovement());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsPathingFailed());
			hash = ReplayCheckpointUnitHandlerHashBool(hash, groundMoveType->IsPathingArrived());
		}
	}

	return hash;
}

static void LogReplayCheckpointUnitHandlerSignature(
	const char* label,
	const std::vector<CUnit*>& activeUnits,
	size_t activeSlowUpdateUnit,
	size_t activeUpdateUnit
) {
	const int debugFrame = configHandler->GetInt("ReplayCheckpointDebugSignatureFrame");

	if (debugFrame < 0 || gs == nullptr || gs->frameNum != debugFrame)
		return;

	const bool debugUnitDetails = (std::strcmp(label, "unit-update-begin") == 0);
	uint32_t unitHash = 0x13572468u;
	for (const CUnit* unit: activeUnits) {
		if (unit == nullptr)
			continue;

		const uint32_t unitDetailHash = ReplayCheckpointUnitHandlerHashUnitMoveState(unit);
		unitHash = ReplayCheckpointUnitHandlerHashUInt(unitHash, unitDetailHash);

		const AMoveType* moveType = unit->moveType;
		const CGroundMoveType* groundMoveType = (moveType != nullptr)? dynamic_cast<const CGroundMoveType*>(moveType): nullptr;

		if (debugUnitDetails) {
			const float3& goalPos = (moveType != nullptr)? moveType->goalPos: ZeroVector;
			const float3& oldPos = (moveType != nullptr)? moveType->oldPos: ZeroVector;
			const float3& oldSlowUpdatePos = (moveType != nullptr)? moveType->oldSlowUpdatePos: ZeroVector;
			const float3& oldCollisionUpdatePos = (moveType != nullptr)? moveType->oldCollisionUpdatePos: ZeroVector;
			const float3 currWayPoint = (groundMoveType != nullptr)? static_cast<float3>(groundMoveType->GetCurrWayPoint()): ZeroVector;
			const float3 nextWayPoint = (groundMoveType != nullptr)? static_cast<float3>(groundMoveType->GetNextWayPoint()): ZeroVector;
			const float3& earlyCurrWayPoint = (groundMoveType != nullptr)? groundMoveType->GetEarlyCurrWayPoint(): ZeroVector;
			const float3& earlyNextWayPoint = (groundMoveType != nullptr)? groundMoveType->GetEarlyNextWayPoint(): ZeroVector;

			LOG("[ReplayCheckpoint][unit-detail] %s frame=%d unit=%d hash=%08x team=%d pos=<%.8g,%.8g,%.8g> speed=<%.8g,%.8g,%.8g,%.8g> heading=%d phys=%u coll=%u goal=<%.8g,%.8g,%.8g> old=<%.8g,%.8g,%.8g> oldSlow=<%.8g,%.8g,%.8g> oldColl=<%.8g,%.8g,%.8g> progress=%d useHeading=%u gmt=%u cwp=<%.8g,%.8g,%.8g> nwp=<%.8g,%.8g,%.8g> ecwp=<%.8g,%.8g,%.8g> enwp=<%.8g,%.8g,%.8g> wanted=%.8g current=%.8g delta=%.8g cdist=%.8g pdist=%.8g path=%u nextPath=%u rev=%u atGoal=%u atEnd=%u lastWp=%u raw=%u failed=%u arrived=%u",
				label,
				gs->frameNum,
				unit->id,
				unitDetailHash,
				unit->team,
				unit->pos.x, unit->pos.y, unit->pos.z,
				unit->speed.x, unit->speed.y, unit->speed.z, unit->speed.w,
				static_cast<int>(unit->heading),
				static_cast<unsigned int>(unit->physicalState),
				static_cast<unsigned int>(unit->collidableState),
				goalPos.x, goalPos.y, goalPos.z,
				oldPos.x, oldPos.y, oldPos.z,
				oldSlowUpdatePos.x, oldSlowUpdatePos.y, oldSlowUpdatePos.z,
				oldCollisionUpdatePos.x, oldCollisionUpdatePos.y, oldCollisionUpdatePos.z,
				(moveType != nullptr)? static_cast<int>(moveType->progressState): -1,
				(moveType != nullptr && moveType->UseHeading())? 1u: 0u,
				(groundMoveType != nullptr)? 1u: 0u,
				currWayPoint.x, currWayPoint.y, currWayPoint.z,
				nextWayPoint.x, nextWayPoint.y, nextWayPoint.z,
				earlyCurrWayPoint.x, earlyCurrWayPoint.y, earlyCurrWayPoint.z,
				earlyNextWayPoint.x, earlyNextWayPoint.y, earlyNextWayPoint.z,
				(groundMoveType != nullptr)? groundMoveType->GetWantedSpeed(): 0.0f,
				(groundMoveType != nullptr)? groundMoveType->GetCurrentSpeed(): 0.0f,
				(groundMoveType != nullptr)? groundMoveType->GetDeltaSpeed(): 0.0f,
				(groundMoveType != nullptr)? groundMoveType->GetCurrWayPointDist(): 0.0f,
				(groundMoveType != nullptr)? groundMoveType->GetPrevWayPointDist(): 0.0f,
				(groundMoveType != nullptr)? groundMoveType->GetPathID(): 0u,
				(groundMoveType != nullptr)? groundMoveType->GetNextPathID(): 0u,
				(groundMoveType != nullptr && groundMoveType->IsReversing())? 1u: 0u,
				(groundMoveType != nullptr && groundMoveType->IsAtGoal())? 1u: 0u,
				(groundMoveType != nullptr && groundMoveType->IsAtEndOfPath())? 1u: 0u,
				(groundMoveType != nullptr && groundMoveType->IsLastWaypoint())? 1u: 0u,
				(groundMoveType != nullptr && groundMoveType->IsUsingRawMovement())? 1u: 0u,
				(groundMoveType != nullptr && groundMoveType->IsPathingFailed())? 1u: 0u,
				(groundMoveType != nullptr && groundMoveType->IsPathingArrived())? 1u: 0u
			);
		}
	}

	uint32_t projectileHash = 0xabcdef01u;
	const auto& syncedProjectiles = projectileHandler.GetActiveProjectiles(true);
	for (const CProjectile* projectile: syncedProjectiles) {
		if (projectile == nullptr)
			continue;

		projectileHash = ReplayCheckpointUnitHandlerHashInt(projectileHash, projectile->id);
		projectileHash = ReplayCheckpointUnitHandlerHashUInt(projectileHash, projectile->GetOwnerID());
		projectileHash = ReplayCheckpointUnitHandlerHashUInt(projectileHash, projectile->GetTeamID());
		projectileHash = ReplayCheckpointUnitHandlerHashFloat3(projectileHash, projectile->pos);
		projectileHash = ReplayCheckpointUnitHandlerHashFloat4(projectileHash, projectile->speed);
	}

	LOG("[ReplayCheckpoint][unit-sig] %s frame=%d sync=%08x rng=%llu units=%u unitHash=%08x unitCursor=%u/%u syncedProjectiles=%u projectileHash=%08x",
		label,
		gs->frameNum,
		CSyncChecker::GetChecksum(),
		static_cast<unsigned long long>(gsRNG.GetGenState()),
		static_cast<unsigned int>(activeUnits.size()),
		unitHash,
		static_cast<unsigned int>(activeSlowUpdateUnit),
		static_cast<unsigned int>(activeUpdateUnit),
		static_cast<unsigned int>(syncedProjectiles.size()),
		projectileHash
	);
}


CR_BIND(CUnitHandler, )
CR_REG_METADATA(CUnitHandler, (
	CR_MEMBER(idPool),

	CR_MEMBER(units),
	CR_MEMBER(unitsByDefs),
	CR_MEMBER(activeUnits),
	CR_MEMBER(unitsToBeRemoved),

	CR_MEMBER(builderCAIs),

	CR_MEMBER(activeSlowUpdateUnit),
	CR_MEMBER(activeUpdateUnit),

	CR_MEMBER(maxUnits),
	CR_MEMBER(maxUnitRadius),

	CR_MEMBER(inUpdateCall)
))



UnitMemPool unitMemPool;

CUnitHandler unitHandler;


CUnit* CUnitHandler::NewUnit(const UnitDef* ud)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// special static builder structures that can always be given
	// move orders (which are passed on to all mobile buildees)
	if (ud->IsFactoryUnit())
		return (unitMemPool.alloc<CFactory>());

	// all other types of non-structure "builders", including hubs and
	// nano-towers (the latter should not have any build-options at all,
	// whereas the former should be unable to build any mobile units)
	if (ud->IsMobileBuilderUnit() || ud->IsStaticBuilderUnit())
		return (unitMemPool.alloc<CBuilder>());

	// static non-builder structures
	if (ud->IsBuildingUnit()) {
		if (ud->IsExtractorUnit())
			return (unitMemPool.alloc<CExtractorBuilding>());

		return (unitMemPool.alloc<CBuilding>());
	}

	// regular mobile unit
	return (unitMemPool.alloc<CUnit>());
}



void CUnitHandler::Init() {
	RECOIL_DETAILED_TRACY_ZONE;
	GroundMoveSystem::Init();
	GeneralMoveSystem::Init();
	UnitTrapCheckSystem::Init();

	{
		// set the global (runtime-constant) unit-limit as the sum
		// of  all team unit-limits, which is *always* <= MAX_UNITS
		// (note that this also counts the Gaia team)
		//
		// teams can not be created at runtime, but they can die and
		// in that case the per-team limit is recalculated for every
		// other team in the respective allyteam
		maxUnits = CalcMaxUnits();
		maxUnitRadius = 0.0f;
	}
	{
		activeSlowUpdateUnit = 0;
		activeUpdateUnit = 0;
	}
	{
		units.resize(maxUnits, nullptr);
		activeUnits.reserve(maxUnits);

		unitMemPool.reserve(128);

		// id's are used as indices, so they must lie in [0, units.size() - 1]
		// (furthermore all id's are treated equally, none have special status)
		idPool.Clear();
		idPool.Expand(0, MAX_UNITS);

		for (int teamNum = 0; teamNum < teamHandler.ActiveTeams(); teamNum++) {
			unitsByDefs[teamNum].resize(unitDefHandler->NumUnitDefs() + 1);
		}
	}
}


void CUnitHandler::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (CUnit* u: activeUnits) {
		// ~CUnit dereferences featureHandler which is destroyed already
		u->KilledScriptFinished(-1);
		unitMemPool.free(u);
	}
	{
		// do not clear in ctor because creg-loaded objects would be wiped out
		unitMemPool.clear();

		units.clear();

		for (int teamNum = 0; teamNum < MAX_TEAMS; teamNum++) {
			// reuse inner vectors when reloading
			// unitsByDefs[teamNum].clear();

			for (size_t defID = 0; defID < unitsByDefs[teamNum].size(); defID++) {
				unitsByDefs[teamNum][defID].clear();
			}
		}

		activeUnits.clear();
		unitsToBeRemoved.clear();

		// only iterated by unsynced code, GetBuilderCAIs has no synced callers
		builderCAIs.clear();
	}
	{
		maxUnits = 0;
		maxUnitRadius = 0.0f;
	}
}


void CUnitHandler::DeleteScripts()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// predelete scripts since they sometimes reference (pieces
	// of) models, which are already gone before KillSimulation
	for (CUnit* u: activeUnits) {
		u->DeleteScript();
	}
}


void CUnitHandler::InsertActiveUnit(CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	idPool.AssignID(unit);

	assert(unit->id < units.size());
	assert(units[unit->id] == nullptr);

	#if 0
	// randomized insertion is supposed to break up peak loads
	// during the (staggered) SlowUpdate step, but also causes
	// more jumping around in memory for regular Updates
	// in larger games (where it would matter most) the order
	// of insertion is essentially guaranteed to be random by
	// interleaved player actions anyway, and if needed could
	// also be achieved by periodic shuffling
	const unsigned int insertionPos = gsRNG.NextInt(activeUnits.size());

	assert(insertionPos < activeUnits.size());
	activeUnits.insert(activeUnits.begin() + insertionPos, unit);

	// do not (slow)update the same unit twice if the new one
	// gets inserted behind our current iterator position and
	// right-shifts the rest
	activeSlowUpdateUnit += (insertionPos <= activeSlowUpdateUnit);
	activeUpdateUnit += (insertionPos <= activeUpdateUnit);

	#else
	activeUnits.push_back(unit);
	#endif

	units[unit->id] = unit;
}


bool CUnitHandler::AddUnit(CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// LoadUnit should make sure this is true
	assert(CanAddUnit(unit->id));

	InsertActiveUnit(unit);
	teamHandler.Team(unit->team)->AddUnit(unit, CTeam::AddBuilt);

	// 0 is not a valid UnitDef id, so just use unitsByDefs[team][0]
	// as an unsorted bin to store all units belonging to unit->team
	spring::VectorInsertUnique(GetUnitsByTeamAndDef(unit->team,                 0), unit, false);
	spring::VectorInsertUnique(GetUnitsByTeamAndDef(unit->team, unit->unitDef->id), unit, false);

	maxUnitRadius = std::max(unit->radius, maxUnitRadius);
	return true;
}


bool CUnitHandler::GarbageCollectUnit(unsigned int id)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (inUpdateCall)
		return false;

	assert(unitsToBeRemoved.empty());

	if (!QueueDeleteUnit(units[id]))
		return false;

	// only processes units[id]
	DeleteUnits();

	return (idPool.RecycleID(id));
}


void CUnitHandler::QueueDeleteUnits()
{
	ZoneScoped;
	// gather up dead units
	for (activeUpdateUnit = 0; activeUpdateUnit < activeUnits.size(); ++activeUpdateUnit) {
		QueueDeleteUnit(activeUnits[activeUpdateUnit]);
	}
}

bool CUnitHandler::QueueDeleteUnit(CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!unit->deathScriptFinished)
		return false;

	// there are many ways to fiddle with "deathScriptFinished", so a unit may
	// arrive here not having been properly killed while isDead is still false
	// make sure we always call Killed; no-op if isDead was already set to true
	unit->ForcedKillUnit(nullptr, false, true);
	unitsToBeRemoved.push_back(unit);
	return true;
}


void CUnitHandler::DeleteUnits()
{
	ZoneScopedC(tracy::Color::Goldenrod);
	while (!unitsToBeRemoved.empty()) {
		DeleteUnit(unitsToBeRemoved.back());
		unitsToBeRemoved.pop_back();
	}
}

void CUnitHandler::DeleteUnit(CUnit* delUnit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(delUnit->isDead);

	// we want to call RenderUnitDestroyed while the unit is still valid
	eventHandler.RenderUnitDestroyed(delUnit);

	const auto it = std::find(activeUnits.begin(), activeUnits.end(), delUnit);

	if (it == activeUnits.end()) {
		assert(false);
		return;
	}

	const int delUnitTeam = delUnit->team;
	const int delUnitType = delUnit->unitDef->id;

	teamHandler.Team(delUnitTeam)->RemoveUnit(delUnit, CTeam::RemoveDied);

	if (activeSlowUpdateUnit > std::distance(activeUnits.begin(), it))
		--activeSlowUpdateUnit;

	activeUnits.erase(it);

	spring::VectorErase(GetUnitsByTeamAndDef(delUnitTeam,           0), delUnit);
	spring::VectorErase(GetUnitsByTeamAndDef(delUnitTeam, delUnitType), delUnit);

	idPool.FreeID(delUnit->id, true);

	units[delUnit->id] = nullptr;

	entt::entity delUnitEntity = delUnit->entityReference;

	CSolidObject::SetDeletingRefID(delUnit->id);
	unitMemPool.free(delUnit);
	CSolidObject::SetDeletingRefID(-1);

	assert( Sim::registry.valid(delUnitEntity) );
	Sim::registry.destroy(delUnitEntity);
}

void CUnitHandler::UpdateUnitMoveTypes()
{
	SCOPED_TIMER("Sim::Unit::MoveType");

	GroundMoveSystem::Update();
	GeneralMoveSystem::Update();
	UnitTrapCheckSystem::Update();
}

void CUnitHandler::UpdateUnitLosStates()
{
	ZoneScopedC(tracy::Color::Goldenrod);
	for (CUnit* unit: activeUnits) {
		for (int at = 0; at < teamHandler.ActiveAllyTeams(); ++at) {
			unit->UpdateLosStatus(at);
		}
	}
}


void CUnitHandler::SlowUpdateUnits()
{
	SCOPED_TIMER("Sim::Unit::SlowUpdate");

	assert(activeSlowUpdateUnit >= 0);

	// reset the iterator every <UNIT_SLOWUPDATE_RATE> frames
	if ((gs->frameNum % UNIT_SLOWUPDATE_RATE) == 0)
		activeSlowUpdateUnit = 0;

	const size_t idxBeg = activeSlowUpdateUnit;
	const size_t maximumCnt = activeUnits.size() - idxBeg;
	const size_t logicalCnt = (activeUnits.size() / UNIT_SLOWUPDATE_RATE) + 1;
	const size_t indCnt = logicalCnt > maximumCnt ? maximumCnt : logicalCnt;
	const size_t idxEnd = idxBeg + indCnt;

	activeSlowUpdateUnit = idxEnd;
	// stagger the SlowUpdate's

	static std::vector<CUnit*> updateBoundingVolumeList;
	updateBoundingVolumeList.clear();
	{
		ZoneScopedN("Sim::Unit::SlowUpdateST");
		for (size_t i = idxBeg; i < idxEnd; ++i) {
			CUnit* unit = activeUnits[i];

			unit->SanityCheck();
			unit->SlowUpdate();
			unit->SlowUpdateWeapons();
			unit->SanityCheck();

			if (!unit->isDead && unit->localModel.GetBoundariesNeedsRecalc())
				updateBoundingVolumeList.emplace_back(unit);
		}
	}
	// Since the bounding volumes are calculated from the maximum piecematrix-offset piece vertices
	// They dont have much of an effect if updated late-ish.
	{
		ZoneScopedN("Sim::Unit::SlowUpdateMT");
		for_mt(0, updateBoundingVolumeList.size(), [](int i) {
			updateBoundingVolumeList[i]->localModel.UpdateBoundingVolume();
		});
	}
}

void CUnitHandler::UpdateUnits()
{
	SCOPED_TIMER("Sim::Unit::Update");

	size_t activeUnitCount = activeUnits.size();
	for (size_t i = 0; i < activeUnitCount; ++i) {
		CUnit* unit = activeUnits[i];

		unit->SanityCheck();
		unit->Update();
		unit->moveType->UpdateCollisionMap();
		// unsynced; done on-demand when drawing unit
		// unit->UpdateLocalModel();
		unit->SanityCheck();

		assert(activeUnits[i] == unit);
	}
}

void CUnitHandler::UpdateUnitWeapons()
{
	{
		SCOPED_TIMER("Sim::Unit::UpdateWeaponVectors");

		for_mt_chunk(0, activeUnits.size(), [&](const int idx) {
			auto unit = activeUnits[idx];
			unit->UpdateWeaponVectors();
		});
	}
	{
		SCOPED_TIMER("Sim::Unit::Weapon");
		for (activeUpdateUnit = 0; activeUpdateUnit < activeUnits.size(); ++activeUpdateUnit) {
			activeUnits[activeUpdateUnit]->UpdateWeapons();
		}
	}
}

void CUnitHandler::UpdatePreFrame()
{
	SCOPED_TIMER("Sim::Unit::UpdatePreFrame");
	inUpdateCall = true;

	for (CUnit* unit : activeUnits) {
		unit->UpdatePrevFrameTransform();
	}

	inUpdateCall = false;
}

void CUnitHandler::Update()
{
	inUpdateCall = true;

	LogReplayCheckpointUnitHandlerSignature("unit-update-begin", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	DeleteUnits();
	LogReplayCheckpointUnitHandlerSignature("after-delete-units", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	UpdateUnitMoveTypes();
	LogReplayCheckpointUnitHandlerSignature("after-movetype-update", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	QueueDeleteUnits();
	LogReplayCheckpointUnitHandlerSignature("after-queue-delete", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	UpdateUnitLosStates();
	LogReplayCheckpointUnitHandlerSignature("after-los-update", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	SlowUpdateUnits();
	LogReplayCheckpointUnitHandlerSignature("after-slow-update", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	UpdateUnits();
	LogReplayCheckpointUnitHandlerSignature("after-unit-body-update", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);
	UpdateUnitWeapons();
	LogReplayCheckpointUnitHandlerSignature("after-weapon-update", activeUnits, activeSlowUpdateUnit, activeUpdateUnit);

	inUpdateCall = false;
}

void CUnitHandler::UpdatePostAnimation()
{
	SCOPED_TIMER("Sim::Unit::UpdatePostAnimation");
	inUpdateCall = true;

	for (auto* unit : activeUnits) {
		unit->UpdateTransportees();
	}

	inUpdateCall = false;
}

void CUnitHandler::AddBuilderCAI(CBuilderCAI* b)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// called from CBuilderCAI --> owner is already valid
	builderCAIs[b->owner->id] = b;
}

void CUnitHandler::RemoveBuilderCAI(CBuilderCAI* b)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// called from ~CUnit --> owner is still valid
	assert(b->owner != nullptr);
	builderCAIs.erase(b->owner->id);
}


void CUnitHandler::ChangeUnitTeam(CUnit* unit, int oldTeamNum, int newTeamNum)
{
	RECOIL_DETAILED_TRACY_ZONE;
	spring::VectorErase       (GetUnitsByTeamAndDef(oldTeamNum,                 0), unit       );
	spring::VectorErase       (GetUnitsByTeamAndDef(oldTeamNum, unit->unitDef->id), unit       );
	spring::VectorInsertUnique(GetUnitsByTeamAndDef(newTeamNum,                 0), unit, false);
	spring::VectorInsertUnique(GetUnitsByTeamAndDef(newTeamNum, unit->unitDef->id), unit, false);
}


bool CUnitHandler::CanBuildUnit(const UnitDef* unitdef, int team) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (teamHandler.Team(team)->AtUnitLimit())
		return false;

	return (NumUnitsByTeamAndDef(team, unitdef->id) < unitdef->maxThisUnit);
}

unsigned int CUnitHandler::CalcMaxUnits() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	unsigned int n = 0;

	for (unsigned int i = 0; i < teamHandler.ActiveTeams(); i++) {
		n += teamHandler.Team(i)->GetMaxUnits();
	}

	return n;
}
