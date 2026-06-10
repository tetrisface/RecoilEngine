/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LosHandler.h"

#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/ModInfo.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/GlobalSynced.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/SpringHash.h"
#include "System/Sync/SyncChecker.h"
#include "System/creg/STL_Deque.h"
#include "System/EventHandler.h"
#include "System/SafeUtil.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"
#ifdef USING_CREG
#include "System/creg/TypeDeduction.h"
#endif

#include "System/Misc/TracyDefs.h"

#include <algorithm>
#include <cstdint>

#define USE_STAGGERED_UPDATES 0



CR_BIND(CLosHandler, )

// ILosTypes aren't creg'ed cause they repopulate themselves in case of loading a saved game
CR_REG_METADATA(CLosHandler,(
	CR_IGNORED(autoLinkEvents),
	CR_IGNORED(autoLinkedEvents),
	CR_IGNORED(name),
	CR_IGNORED(order),
	CR_IGNORED(synced_),

	CR_MEMBER(globalLOS),
	CR_IGNORED(los),
	CR_IGNORED(airLos),
	CR_IGNORED(radar),
	CR_IGNORED(sonar),
	CR_IGNORED(seismic),
	CR_IGNORED(jammer),
	CR_IGNORED(sonarJammer),
	CR_MEMBER(baseRadarErrorSize),
	CR_MEMBER(baseRadarErrorMult),
	CR_MEMBER(radarErrorSizes),
	CR_IGNORED(losTypes)
))



//////////////////////////////////////////////////////////////////////
// SLosInstance
//////////////////////////////////////////////////////////////////////

inline void SLosInstance::Init(int radius, int allyteam, int2 basePos, float baseHeight, int hashNum)
{
	this->allyteam = allyteam;
	this->radius = radius;
	this->basePos = basePos;
	this->baseHeight = baseHeight;
	this->refCount = 0;
	this->hashNum = hashNum;
	this->status = NONE;
	this->isCached = false;
	this->isQueuedForUpdate = false;
	this->isQueuedForTerraform = false;
}


//////////////////////////////////////////////////////////////////////
// ILosType
//////////////////////////////////////////////////////////////////////

size_t ILosType::cacheFails = 1;
size_t ILosType::cacheHits  = 1;
size_t ILosType::cacheRefs  = 1;

constexpr float CLosHandler::defBaseRadarErrorSize;
constexpr float CLosHandler::defBaseRadarErrorMult;
constexpr SLosInstance::RLE SLosInstance::EMPTY_RLE;

using ReplayCheckpointLosMapSnapshot = std::vector<std::vector<std::vector<unsigned short>>>;

static ReplayCheckpointLosMapSnapshot CaptureReplayCheckpointLosMaps(const std::array<ILosType*, 7>& losTypes)
{
	ReplayCheckpointLosMapSnapshot maps;
	maps.resize(losTypes.size());

	for (size_t typeIdx = 0; typeIdx < losTypes.size(); ++typeIdx) {
		const ILosType* lt = losTypes[typeIdx];
		if (lt == nullptr)
			continue;

		maps[typeIdx].resize(lt->losMaps.size());

		for (size_t allyTeamIdx = 0; allyTeamIdx < lt->losMaps.size(); ++allyTeamIdx) {
			maps[typeIdx][allyTeamIdx] = lt->losMaps[allyTeamIdx].GetLosMap();
		}
	}

	return maps;
}

static void RestoreReplayCheckpointLosMaps(const ReplayCheckpointLosMapSnapshot& maps, const std::array<ILosType*, 7>& losTypes)
{
	for (size_t typeIdx = 0; typeIdx < std::min(maps.size(), losTypes.size()); ++typeIdx) {
		ILosType* lt = losTypes[typeIdx];
		if (lt == nullptr)
			continue;

		for (size_t allyTeamIdx = 0; allyTeamIdx < std::min(maps[typeIdx].size(), lt->losMaps.size()); ++allyTeamIdx) {
			const auto& savedMap = maps[typeIdx][allyTeamIdx];
			if (savedMap.size() != lt->losMaps[allyTeamIdx].GetLosMap().size())
				continue;

			lt->losMaps[allyTeamIdx].SetLosMap(savedMap);
		}
	}
}

static uint32_t ReplayCheckpointLosHashInt(uint32_t hash, int value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointLosHashUInt(uint32_t hash, uint32_t value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointLosHashFloat(uint32_t hash, float value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointLosHashInt2(uint32_t hash, const int2& value)
{
	hash = ReplayCheckpointLosHashInt(hash, value.x);
	hash = ReplayCheckpointLosHashInt(hash, value.y);
	return hash;
}

template<typename T>
static uint32_t ReplayCheckpointLosHashVectorRaw(uint32_t hash, const std::vector<T>& values)
{
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(values.size()));
	if (!values.empty())
		hash = spring::LiteHash(values.data(), static_cast<unsigned>(values.size() * sizeof(T)), hash);
	return hash;
}

static uint32_t ReplayCheckpointLosHashInstancePtr(uint32_t hash, const SLosInstance* instance)
{
	return ReplayCheckpointLosHashInt(hash, (instance != nullptr) ? instance->id : -1);
}

static uint32_t ReplayCheckpointLosHashInstanceQueue(uint32_t hash, const std::deque<SLosInstance*>& queue)
{
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(queue.size()));

	for (const SLosInstance* instance: queue)
		hash = ReplayCheckpointLosHashInstancePtr(hash, instance);

	return hash;
}

#ifdef USING_CREG
namespace {
	static void SerializeReplayCheckpointBoolByte(creg::ISerializer* s, bool& value)
	{
		uint8_t storedValue = value ? 1u : 0u;
		s->Serialize(storedValue);
		if (!s->IsWriting())
			value = (storedValue != 0u);
	}

	template<typename T>
	static void SerializeReplayCheckpointVectorRaw(creg::ISerializer* s, std::vector<T>& values)
	{
		uint32_t valueCount = static_cast<uint32_t>(values.size());
		s->Serialize(valueCount);
		if (!s->IsWriting())
			values.resize(valueCount);
		if (!values.empty())
			s->Serialize(values.data(), static_cast<int>(values.size() * sizeof(T)));
	}

	static int32_t GetReplayCheckpointLosInstanceId(const ILosType& losType, const SLosInstance* instance)
	{
		if (instance == nullptr)
			return -1;

		assert(instance->id >= 0);
		assert(static_cast<size_t>(instance->id) < losType.instances.size());
		assert(&losType.instances[instance->id] == instance);
		return instance->id;
	}

	static SLosInstance* GetReplayCheckpointLosInstance(ILosType& losType, int32_t instanceId)
	{
		if (instanceId < 0)
			return nullptr;
		if (static_cast<size_t>(instanceId) >= losType.instances.size())
			return nullptr;

		return &losType.instances[instanceId];
	}

	static void SerializeReplayCheckpointLosInstancePtrDeque(
		creg::ISerializer* s,
		ILosType& losType,
		std::deque<SLosInstance*>& queue
	)
	{
		uint32_t queueSize = static_cast<uint32_t>(queue.size());
		s->Serialize(queueSize);

		if (!s->IsWriting())
			queue.clear();

		for (uint32_t i = 0; i < queueSize; ++i) {
			int32_t instanceId = s->IsWriting() ? GetReplayCheckpointLosInstanceId(losType, queue[i]) : -1;
			s->Serialize(instanceId);

			if (s->IsWriting())
				continue;

			if (SLosInstance* instance = GetReplayCheckpointLosInstance(losType, instanceId); instance != nullptr)
				queue.push_back(instance);
		}
	}

	static void SerializeReplayCheckpointUnitLosLinks(
		creg::ISerializer* s,
		const std::array<ILosType*, 7>& losTypes
	)
	{
		uint32_t unitCount = static_cast<uint32_t>(unitHandler.GetActiveUnits().size());
		s->Serialize(unitCount);

		if (!s->IsWriting()) {
			for (CUnit* unit: unitHandler.GetActiveUnits()) {
				unit->los.fill(nullptr);
			}
		}

		for (uint32_t unitIdx = 0; unitIdx < unitCount; ++unitIdx) {
			int32_t unitId = -1;
			CUnit* unit = nullptr;

			if (s->IsWriting()) {
				unit = unitHandler.GetActiveUnits()[unitIdx];
				unitId = unit->id;
			}

			s->Serialize(unitId);

			if (!s->IsWriting())
				unit = unitHandler.GetUnit(unitId);

			for (size_t typeIdx = 0; typeIdx < losTypes.size(); ++typeIdx) {
				ILosType* losType = losTypes[typeIdx];
				int32_t instanceId = -1;

				if (s->IsWriting() && unit != nullptr && losType != nullptr)
					instanceId = GetReplayCheckpointLosInstanceId(*losType, unit->los[typeIdx]);

				s->Serialize(instanceId);

				if (s->IsWriting() || unit == nullptr || losType == nullptr)
					continue;

				unit->los[typeIdx] = GetReplayCheckpointLosInstance(*losType, instanceId);
			}
		}
	}
}
#endif


void SLosInstance::SerializeReplayCheckpoint(creg::ISerializer* s)
{
#ifdef USING_CREG
	s->Serialize(id);
	s->Serialize(allyteam);
	s->Serialize(radius);
	s->Serialize(&basePos, sizeof(basePos));
	s->Serialize(baseHeight);
	s->Serialize(refCount);
	SerializeReplayCheckpointVectorRaw(s, squares);
	s->Serialize(hashNum);
	s->Serialize(status);
	SerializeReplayCheckpointBoolByte(s, isCached);
	SerializeReplayCheckpointBoolByte(s, isQueuedForUpdate);
	SerializeReplayCheckpointBoolByte(s, isQueuedForTerraform);
#endif
}


void ILosType::SerializeReplayCheckpoint(creg::ISerializer* s)
{
#ifdef USING_CREG
	int typeValue = static_cast<int>(type);
	int algoTypeValue = static_cast<int>(algoType);

	s->Serialize(mipLevel);
	s->Serialize(mipDiv);
	s->Serialize(invDiv);
	s->Serialize(&size, sizeof(size));
	s->Serialize(typeValue);
	s->Serialize(algoTypeValue);

	if (!s->IsWriting()) {
		type = static_cast<LosType>(typeValue);
		algoType = static_cast<LosAlgoType>(algoTypeValue);
	}

	uint32_t losMapCount = static_cast<uint32_t>(losMaps.size());
	s->Serialize(losMapCount);

	for (uint32_t mapIdx = 0; mapIdx < losMapCount; ++mapIdx) {
		std::vector<unsigned short> mapData;
		if (s->IsWriting() && mapIdx < losMaps.size())
			mapData = losMaps[mapIdx].GetLosMap();

		SerializeReplayCheckpointVectorRaw(s, mapData);

		if (s->IsWriting() || mapIdx >= losMaps.size())
			continue;
		if (mapData.size() != losMaps[mapIdx].GetLosMap().size())
			continue;

		losMaps[mapIdx].SetLosMap(mapData);
	}

	uint32_t instanceCount = static_cast<uint32_t>(instances.size());
	s->Serialize(instanceCount);

	if (!s->IsWriting()) {
		instances.clear();
		for (uint32_t instanceIdx = 0; instanceIdx < instanceCount; ++instanceIdx) {
			instances.emplace_back(instanceIdx);
		}
	}

	for (SLosInstance& instance: instances) {
		instance.SerializeReplayCheckpoint(s);
	}

	SerializeReplayCheckpointVectorRaw(s, freeIDs);

	uint32_t hashCount = static_cast<uint32_t>(instanceHashes.size());
	s->Serialize(hashCount);

	if (s->IsWriting()) {
		for (const auto& [hashNum, hashInstances]: instanceHashes) {
			int storedHashNum = hashNum;
			uint32_t instanceIdCount = static_cast<uint32_t>(hashInstances.size());

			s->Serialize(storedHashNum);
			s->Serialize(instanceIdCount);

			for (SLosInstance* instance: hashInstances) {
				int32_t instanceId = GetReplayCheckpointLosInstanceId(*this, instance);
				s->Serialize(instanceId);
			}
		}
	} else {
		instanceHashes.clear();

		for (uint32_t hashIdx = 0; hashIdx < hashCount; ++hashIdx) {
			int hashNum = 0;
			uint32_t instanceIdCount = 0;

			s->Serialize(hashNum);
			s->Serialize(instanceIdCount);

			auto& hashInstances = instanceHashes[hashNum];
			for (uint32_t idIdx = 0; idIdx < instanceIdCount; ++idIdx) {
				int32_t instanceId = -1;
				s->Serialize(instanceId);

				if (SLosInstance* instance = GetReplayCheckpointLosInstance(*this, instanceId); instance != nullptr)
					hashInstances.push_back(instance);
			}
		}
	}

	const auto serializeDelayedQueue = [s, this](std::deque<DelayedInstance>& queue) {
		uint32_t queueSize = static_cast<uint32_t>(queue.size());
		s->Serialize(queueSize);

		if (!s->IsWriting())
			queue.clear();

		for (uint32_t i = 0; i < queueSize; ++i) {
			int32_t instanceId = -1;
			int timeoutTime = 0;

			if (s->IsWriting()) {
				instanceId = GetReplayCheckpointLosInstanceId(*this, queue[i].instance);
				timeoutTime = queue[i].timeoutTime;
			}

			s->Serialize(instanceId);
			s->Serialize(timeoutTime);

			if (s->IsWriting())
				continue;

			if (SLosInstance* instance = GetReplayCheckpointLosInstance(*this, instanceId); instance != nullptr)
				queue.push_back({instance, timeoutTime});
		}
	};

	serializeDelayedQueue(delayedDeleteQue);
	serializeDelayedQueue(delayedTerraQue);
	SerializeReplayCheckpointLosInstancePtrDeque(s, *this, losUpdate);
	SerializeReplayCheckpointLosInstancePtrDeque(s, *this, losCache);

	if (!s->IsWriting()) {
		losRemove.clear();
		losAdd.clear();
		losDeleted.clear();
		losRecalc.clear();
	}
#endif
}

uint32_t ILosType::GetReplayCheckpointMapHash() const
{
	uint32_t hash = 0x1c0ffee5u;

	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(losMaps.size()));
	for (const CLosMap& losMap: losMaps) {
		hash = ReplayCheckpointLosHashVectorRaw(hash, losMap.GetLosMap());
	}

	return hash;
}

uint32_t ILosType::GetReplayCheckpointQueueHash() const
{
	uint32_t hash = 0x105cafe1u;

	const auto hashDelayedQueue = [](uint32_t queueHash, const std::deque<DelayedInstance>& queue) {
		queueHash = ReplayCheckpointLosHashUInt(queueHash, static_cast<uint32_t>(queue.size()));

		for (const DelayedInstance& item: queue) {
			queueHash = ReplayCheckpointLosHashInstancePtr(queueHash, item.instance);
			queueHash = ReplayCheckpointLosHashInt(queueHash, item.timeoutTime);
		}

		return queueHash;
	};

	hash = hashDelayedQueue(hash, delayedDeleteQue);
	hash = hashDelayedQueue(hash, delayedTerraQue);
	hash = ReplayCheckpointLosHashInstanceQueue(hash, losUpdate);
	hash = ReplayCheckpointLosHashInstanceQueue(hash, losCache);

	return hash;
}

uint32_t ILosType::GetReplayCheckpointStateHash() const
{
	uint32_t hash = 0x70551eafU;

	hash = ReplayCheckpointLosHashInt(hash, mipLevel);
	hash = ReplayCheckpointLosHashInt(hash, mipDiv);
	hash = ReplayCheckpointLosHashFloat(hash, invDiv);
	hash = ReplayCheckpointLosHashInt2(hash, size);
	hash = ReplayCheckpointLosHashInt(hash, static_cast<int>(type));
	hash = ReplayCheckpointLosHashInt(hash, static_cast<int>(algoType));
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(losMaps.size()));
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(instances.size()));
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(freeIDs.size()));

	hash = ReplayCheckpointLosHashUInt(hash, GetReplayCheckpointMapHash());

	for (size_t instanceIdx = 0; instanceIdx < instances.size(); ++instanceIdx) {
		const SLosInstance& instance = instances[instanceIdx];

		hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(instanceIdx));
		hash = ReplayCheckpointLosHashInt(hash, instance.id);
		hash = ReplayCheckpointLosHashInt(hash, instance.allyteam);
		hash = ReplayCheckpointLosHashInt(hash, instance.radius);
		hash = ReplayCheckpointLosHashInt2(hash, instance.basePos);
		hash = ReplayCheckpointLosHashFloat(hash, instance.baseHeight);
		hash = ReplayCheckpointLosHashInt(hash, instance.refCount);
		hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(instance.squares.size()));
		for (const SLosInstance::RLE& square: instance.squares) {
			hash = ReplayCheckpointLosHashInt(hash, square.start);
			hash = ReplayCheckpointLosHashUInt(hash, square.length);
		}
		hash = ReplayCheckpointLosHashInt(hash, instance.hashNum);
		hash = ReplayCheckpointLosHashInt(hash, instance.status);
		hash = ReplayCheckpointLosHashUInt(hash, instance.isCached ? 1u : 0u);
		hash = ReplayCheckpointLosHashUInt(hash, instance.isQueuedForUpdate ? 1u : 0u);
		hash = ReplayCheckpointLosHashUInt(hash, instance.isQueuedForTerraform ? 1u : 0u);
	}

	hash = ReplayCheckpointLosHashVectorRaw(hash, freeIDs);

	std::vector<std::pair<int, std::vector<int>>> hashEntries;
	hashEntries.reserve(instanceHashes.size());
	for (const auto& [hashNum, hashInstances]: instanceHashes) {
		std::vector<int> instanceIds;
		instanceIds.reserve(hashInstances.size());
		for (const SLosInstance* instance: hashInstances)
			instanceIds.push_back((instance != nullptr) ? instance->id : -1);

		hashEntries.emplace_back(hashNum, std::move(instanceIds));
	}

	std::sort(hashEntries.begin(), hashEntries.end(), [](const auto& lhs, const auto& rhs) {
		return (lhs.first < rhs.first);
	});

	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(hashEntries.size()));
	for (const auto& [hashNum, instanceIds]: hashEntries) {
		hash = ReplayCheckpointLosHashInt(hash, hashNum);
		hash = ReplayCheckpointLosHashVectorRaw(hash, instanceIds);
	}

	hash = ReplayCheckpointLosHashUInt(hash, GetReplayCheckpointQueueHash());
	return hash;
}


void ILosType::Init(const int mipLevel_, LosType type_)
{
	RECOIL_DETAILED_TRACY_ZONE;
	mipLevel = mipLevel_;
	mipDiv = SQUARE_SIZE * (1 << mipLevel);
	invDiv = 1.0f / mipDiv;
	size = {std::max(1, mapDims.mapx >> mipLevel), std::max(1, mapDims.mapy >> mipLevel)};

	type = type_;
	algoType = ((type == LOS_TYPE_LOS || type == LOS_TYPE_RADAR) ? LOS_ALGO_RAYCAST : LOS_ALGO_CIRCLE);

	freeIDs.reserve(4096);
	losMaps.resize(teamHandler.ActiveAllyTeams());

	const float* ctrHeightMap = readMap->GetCenterHeightMapSynced();
	const float* mipHeightMap = readMap->GetMIPHeightMapSynced(mipLevel_);

	for (CLosMap& losMap: losMaps) {
		losMap.Init(size, int2(mapDims.mapx, mapDims.mapy), ctrHeightMap, mipHeightMap, type == LOS_TYPE_LOS);
	}
}

void ILosType::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// iterated in UpdateHeightMapSynced
	spring::clear_unordered_map(instanceHashes);

	// reuse inner vectors when reloading
	// losMaps.clear();
	for (CLosMap& losMap: losMaps) {
		losMap.Kill();
	}

	instances.clear();
	freeIDs.clear();

	delayedDeleteQue.clear();
	delayedTerraQue.clear();
	losUpdate.clear();
	losCache.clear();

	losRemove.clear();
	losAdd.clear();
	losDeleted.clear();
	losRecalc.clear();

	// mark as invalid
	size = {0, 0};
}


float ILosType::GetRadius(const CUnit* unit) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	switch (type) {
		case LOS_TYPE_LOS:          return (unit->losRadius      / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_AIRLOS:       return (unit->airLosRadius   / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_RADAR:        return (unit->radarRadius    / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_SONAR:        return (unit->sonarRadius    / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_JAMMER:       return (unit->jammerRadius   / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_SEISMIC:      return (unit->seismicRadius  / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_SONAR_JAMMER: return (unit->sonarJamRadius / SQUARE_SIZE) >> mipLevel;
		case LOS_TYPE_COUNT:        break; //make the compiler happy
	}
	assert(false);
	return 0.0f;
}


float ILosType::GetHeight(const CUnit* unit) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (algoType == LOS_ALGO_CIRCLE)
		return 0.0f;

	const float emitHeight = (type == LOS_TYPE_LOS || type == LOS_TYPE_AIRLOS) ? unit->unitDef->losHeight : unit->unitDef->radarHeight;
	const float losHeight  = std::max(unit->midPos.y + emitHeight, 0.0f);
	const int bucketSize   = 1 << (mipLevel + 2);
	const float iLosHeight = (int(losHeight) / bucketSize + 0.5f) * bucketSize; // save losHeight in buckets
	return iLosHeight;
}


inline void ILosType::UpdateUnit(CUnit* unit, bool ignore)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// do not check if the unit is inside a transporter here
	// non-firebase transporters stun their cargo, so are already handled below
	// firebase transporters should not deprive sensor coverage from their cargo
	if (unit->isDead || unit->beingBuilt)
		return;

	// NOTE:
	//   when stunned, units can in principle still be given on/off commands
	//   this creates an exploit via Unit::Activate if a unit is a !firebase
	//   transported radar/jammer (it would leave a detached sensor coverage
	//   zone behind at its old position)
	const bool sightOnly = (type == LOS_TYPE_LOS) || (type == LOS_TYPE_AIRLOS);
	const bool noSensors = (!unit->activated || unit->IsStunned());
	if (!sightOnly && noSensors) {
		// block any type of radar/jammer coverage when deactivated
		RemoveUnit(unit);
		return;
	}

	/* No point processing LoS, but units can still be cloaked
	 * or underwater, so the other sensors must still be handled */
	if (sightOnly && losHandler->GetGlobalLOS(unit->allyteam))
		return;

	SLosInstance* uli = unit->los[type];

	#if (USE_STAGGERED_UPDATES == 1)
	if (ignore && uli != nullptr) {
		// make sure ILosType::Update will do nothing with this instance
		uli->status = SLosInstance::TLosStatus::NONE;
		return;
	}
	#endif

	const float3 losPos = unit->midPos;
	const float radius = GetRadius(unit);
	const float height = GetHeight(unit);
	const int2 baseLos = PosToSquare(losPos);
	      int allyteam = unit->allyteam;

	// jammers share all the same map independent of the allyTeam
	if (type == LOS_TYPE_JAMMER || type == LOS_TYPE_SONAR_JAMMER)
		allyteam *= modInfo.separateJammers;

	if (radius <= 0.0f) {
		if (uli != nullptr) {
			unit->los[type] = nullptr;
			UnrefInstance(uli);
		}
		return;
	}

	const auto CanRefInstance = [&](SLosInstance* li) -> bool {
		return (li != nullptr
		    && (li->basePos    == baseLos)
		    && (li->baseHeight == height)
		    && (li->radius     == radius)
		    && (li->allyteam   == allyteam)
		);
	};

	// unchanged?
	if (CanRefInstance(uli))
		return;

	if (uli != nullptr) {
		unit->los[type] = nullptr;
		UnrefInstance(uli);
	}

	const int hash = GetHashNum(unit->allyteam, baseLos, radius);

	// Cache - search if there is already an instance with same properties
	auto vit = instanceHashes.find(hash);

	if (vit != instanceHashes.end()) {
		for (SLosInstance* li: vit->second) {
			if (CanRefInstance(li)) {
				cacheHits += (algoType == LOS_ALGO_RAYCAST);
				unit->los[type] = li;
				RefInstance(li);
				return;
			}
		}
	}

	// New - create a new one
	cacheFails += (algoType == LOS_ALGO_RAYCAST);
	SLosInstance* li = CreateInstance();
	li->Init(radius, allyteam, baseLos, height, hash);
	li->refCount++;
	unit->los[type] = li;
	instanceHashes[hash].push_back(li);
	UpdateInstanceStatus(li, SLosInstance::TLosStatus::NEW);
}


inline void ILosType::RemoveUnit(CUnit* unit, bool delayed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (unit->los[type] == nullptr)
		return;

	if (delayed) {
		DelayedUnrefInstance(unit->los[type]);
	} else {
		UnrefInstance(unit->los[type]);
	}
	unit->los[type] = nullptr;
}


inline void ILosType::LosAdd(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(li);
	assert(teamHandler.IsValidAllyTeam(li->allyteam));

	if (algoType == LOS_ALGO_RAYCAST) {
		losMaps[li->allyteam].AddRaycast(li, 1);
	} else {
		losMaps[li->allyteam].AddCircle(li, 1);
	}
}


inline void ILosType::LosRemove(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (algoType == LOS_ALGO_RAYCAST) {
		losMaps[li->allyteam].AddRaycast(li, -1);
	} else {
		losMaps[li->allyteam].AddCircle(li, -1);
	}
}


inline void ILosType::RefInstance(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if ((++li->refCount) != 1)
		return;

	if (li->isCached) {
		// reactivate cached instance
		cacheRefs += (algoType == LOS_ALGO_RAYCAST);
		auto it = std::find(losCache.begin(), losCache.end(), li);
		li->isCached = false;
		losCache.erase(it);
	}

	UpdateInstanceStatus(li, SLosInstance::TLosStatus::REACTIVATE);
}


void ILosType::UnrefInstance(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(li->refCount > 0);

	if ((--li->refCount) > 0)
		return;

	UpdateInstanceStatus(li, SLosInstance::TLosStatus::REMOVE);
}


inline void ILosType::DelayedUnrefInstance(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	DelayedInstance di;
	di.instance = li;
	di.timeoutTime = (gs->frameNum + (GAME_SPEED + (GAME_SPEED >> 1)));
	delayedDeleteQue.push_back(di);
}


inline void ILosType::AddInstanceToCache(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (li->status & SLosInstance::TLosStatus::RECALC) {
		assert(!li->isCached);
		DeleteInstance(li);
		return;
	}

	li->isCached = true;
	losCache.push_back(li);
}


inline SLosInstance* ILosType::CreateInstance()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!freeIDs.empty()) {
		const int id = freeIDs.back();
		freeIDs.pop_back();
		return &instances[id];
	}

	instances.emplace_back(instances.size());
	return &instances.back();
}


inline void ILosType::DeleteInstance(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(li->refCount == 0);

	auto  pit = instanceHashes.find(li->hashNum); assert(pit != instanceHashes.end());
	auto& vec = pit->second;
	auto  vit = std::find(vec.begin(), vec.end(), li); assert(vit != vec.end());

	*vit = vec.back();
	vec.pop_back();

	// caller has to do that
	assert(!li->isCached);
	/*if (li->isCached) {
		auto it = std::find(losCache.begin(), losCache.end(), li);
		losCache.erase(it);
	}*/

	if (li->isQueuedForTerraform) {
		const auto pred = [&](const DelayedInstance& inst) { return (inst.instance == li); };
		const auto iter = std::find_if(delayedTerraQue.begin(), delayedTerraQue.end(), pred);

		if (iter != delayedTerraQue.end())
			delayedTerraQue.erase(iter);

		li->isQueuedForTerraform = false;
	}

	li->squares.clear();
	freeIDs.push_back(li->id);
}


inline void ILosType::UpdateInstanceStatus(SLosInstance* li, SLosInstance::TLosStatus status)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// queue for update
	if (status == SLosInstance::TLosStatus::RECALC) {
		if (!li->isQueuedForTerraform && !li->isQueuedForUpdate) {
			li->isQueuedForTerraform = true;

			DelayedInstance di;
			di.instance = li;
			di.timeoutTime = (gs->frameNum + 2 * GAME_SPEED);
			delayedTerraQue.push_back(di);
		}
	} else {
		if (!li->isQueuedForUpdate) {
			li->isQueuedForUpdate = true;
			losUpdate.push_back(li);
		}
	}


	// mark the type of update needed
	assert((li->status & status) == 0 || status == SLosInstance::TLosStatus::RECALC);
	li->status |= status;


	// sanity checks (debug only)
	constexpr auto b = SLosInstance::TLosStatus::REACTIVATE | SLosInstance::TLosStatus::RECALC;
	assert((li->status & b) != b || (li->status & SLosInstance::TLosStatus::REMOVE));

	constexpr auto c = SLosInstance::TLosStatus::NEW | SLosInstance::TLosStatus::RECALC;
	assert((li->status & c) != c);

	constexpr auto d = SLosInstance::TLosStatus::NEW | SLosInstance::TLosStatus::REACTIVATE;
	assert((li->status & d) != d);

	constexpr auto e = SLosInstance::TLosStatus::NEW | SLosInstance::TLosStatus::REMOVE;
	assert((li->status & e) != e);

	if (li->status & SLosInstance::TLosStatus::RECALC)
		assert(li->refCount > 0 || (li->status & SLosInstance::TLosStatus::REMOVE));

	if (li->refCount == 0)
		assert(li->isCached || (li->status & SLosInstance::TLosStatus::REMOVE));

	if (status == SLosInstance::TLosStatus::REMOVE)
		assert(li->refCount == 0);
}


inline SLosInstance::TLosStatus ILosType::OptimizeInstanceUpdate(SLosInstance* li)
{
	RECOIL_DETAILED_TRACY_ZONE;
	constexpr auto a = SLosInstance::TLosStatus::REACTIVATE | SLosInstance::TLosStatus::REMOVE;
	if ((li->status & a) == a) {
		assert(li->refCount > 0);
		li->status &= ~a;
	}

	if (li->status & SLosInstance::TLosStatus::NEW) {
		assert(li->refCount > 0);
		return SLosInstance::TLosStatus::NEW;
	}
	if (li->status & SLosInstance::TLosStatus::REMOVE) {
		assert(li->refCount == 0);
		return SLosInstance::TLosStatus::REMOVE;
	}
	if (li->status & SLosInstance::TLosStatus::REACTIVATE) {
		assert(li->refCount > 0);
		return SLosInstance::TLosStatus::REACTIVATE;
	}
	if (li->status & SLosInstance::TLosStatus::RECALC) {
		assert(li->refCount > 0);
		return SLosInstance::TLosStatus::RECALC;
	}
	return SLosInstance::TLosStatus::NONE;
}


inline int ILosType::GetHashNum(const int allyteam, const int2 baseLos, const float radius) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	std::uint32_t hash = 0;
	hash = spring::LiteHash(&allyteam, sizeof(allyteam), hash);
	hash = spring::LiteHash(&baseLos,  sizeof(baseLos),  hash);
	hash = spring::LiteHash(&radius,   sizeof(radius),   hash);
	return hash;
}


void ILosType::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// delayed delete
	while (!delayedDeleteQue.empty() && delayedDeleteQue.front().timeoutTime < gs->frameNum) {
		UnrefInstance(delayedDeleteQue.front().instance);
		delayedDeleteQue.pop_front();
	}

	// relos after terraform is delayed
	while (!delayedTerraQue.empty() && delayedTerraQue.front().timeoutTime < gs->frameNum) {
		SLosInstance* li = delayedTerraQue.front().instance;
		li->isQueuedForTerraform = false;
		if (!li->isQueuedForUpdate)
			losUpdate.push_back(li);

		delayedTerraQue.pop_front();
	}

	// no updates? -> early exit
	if (losUpdate.empty())
		return;


	losRemove.clear();
	losRemove.reserve(losUpdate.size());
	losAdd.clear();
	losAdd.reserve(losUpdate.size());
	losDeleted.clear();
	losDeleted.reserve(losUpdate.size());

	if (algoType == LOS_ALGO_RAYCAST) {
		losRecalc.clear();
		losRecalc.reserve(losUpdate.size());
	}

	// filter the updates into their subparts
	for (SLosInstance* li: losUpdate) {
		const auto status = OptimizeInstanceUpdate(li);
		li->isQueuedForUpdate = false;

		switch (status) {
			case SLosInstance::TLosStatus::NEW: {
				if (algoType == LOS_ALGO_RAYCAST) losRecalc.push_back(li);
				losAdd.push_back(li);
			} break;
			case SLosInstance::TLosStatus::REACTIVATE: {
				losAdd.push_back(li);
			} break;
			case SLosInstance::TLosStatus::RECALC: {
				losRemove.push_back(li);
				if (algoType == LOS_ALGO_RAYCAST) losRecalc.push_back(li);
				losAdd.push_back(li);
			} break;
			case SLosInstance::TLosStatus::REMOVE: {
				losRemove.push_back(li);
				losDeleted.push_back(li);
			} break;
			case SLosInstance::TLosStatus::NONE: {
			} break;
			default: assert(false);
		}

		if (status == SLosInstance::TLosStatus::REMOVE) {
			// clear all bits except recalc
			// so the instance gets deleted right away in AddInstanceToCache()
			li->status &= SLosInstance::TLosStatus::RECALC;
		} else {
			li->status = SLosInstance::TLosStatus::NONE;
		}
	}

	// remove sight
	//FIXME multithread?
	for (SLosInstance* li: losRemove) {
		LosRemove(li);
	}

	// raycast terrain
	if (algoType == LOS_ALGO_RAYCAST)  {
		for_mt(0, losRecalc.size(), [&](const int idx) {
			auto li = losRecalc[idx];
			assert(li->refCount > 0);
			li->squares.clear();
			losMaps[li->allyteam].PrepareRaycast(li);
		});
	}

	// add sight
	for (SLosInstance* li: losAdd) {
		assert(li->refCount > 0);
		LosAdd(li);
	}

	// delete / move to cache unused instances
	if (algoType == LOS_ALGO_RAYCAST) {
		while (!losCache.empty() && ((losCache.size() + losDeleted.size()) > CACHE_SIZE)) {
			SLosInstance* li = losCache.front();
			losCache.pop_front();
			li->isCached = false;
			DeleteInstance(li);
		}

		for (SLosInstance* li: losDeleted) {
			assert(li->refCount == 0);
			AddInstanceToCache(li);
		}
	} else {
		assert(losCache.empty());
		for (SLosInstance* li: losDeleted) {
			DeleteInstance(li);
		}
	}

	losUpdate.clear();
}


void ILosType::UpdateHeightMapSynced(SRectangle rect)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (algoType == LOS_ALGO_CIRCLE)
		return;

	auto CheckOverlap = [&](SLosInstance* li, SRectangle rect) -> bool {
		int2 pos = li->basePos * mipDiv;
		const int radius = li->radius * mipDiv;

		const int hw = rect.GetWidth() * (SQUARE_SIZE / 2);
		const int hh = rect.GetHeight() * (SQUARE_SIZE / 2);

		int2 circleDistance;
		circleDistance.x = std::abs(pos.x - rect.x1 * SQUARE_SIZE) - hw;
		circleDistance.y = std::abs(pos.y - rect.y1 * SQUARE_SIZE) - hh;

		if (circleDistance.x > radius) { return false; }
		if (circleDistance.y > radius) { return false; }
		if (circleDistance.x <= 0) { return true; }
		if (circleDistance.y <= 0) { return true; }

		return (Square(circleDistance.x) + Square(circleDistance.y)) <= Square(radius);
	};

	// delete unused instances that overlap with the changed rectangle
	for (auto it = losCache.begin(); it != losCache.end();) {
		SLosInstance* li = *it;
		if (li->refCount > 0 || !CheckOverlap(li, rect)) {
			++it;
			continue;
		}

		it = losCache.erase(it);
		li->isCached = false;
		DeleteInstance(li);
	}

	// relos used instances
	for (auto& p: instanceHashes) {
		for (SLosInstance* li: p.second) {
			if (li->status & SLosInstance::TLosStatus::RECALC)
				continue;
			if (!CheckOverlap(li, rect))
				continue;

			UpdateInstanceStatus(li, SLosInstance::TLosStatus::RECALC);
		}
	}
}




//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

// CLosHandler is an EventClient, can not construct in global scope
alignas(CLosHandler) static std::byte losHandlerMem[sizeof(CLosHandler)];

CLosHandler* losHandler = nullptr;


void CLosHandler::InitStatic()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// reset globals
	ILosType::cacheFails = 1;
	ILosType::cacheHits  = 1;
	ILosType::cacheRefs  = 1;

	if (losHandler == nullptr)
		losHandler = new (losHandlerMem) CLosHandler();

	losHandler->Init();
}

void CLosHandler::KillStatic(bool reload)
{
	RECOIL_DETAILED_TRACY_ZONE;
	losHandler->Kill();

	if (reload)
		return;

	spring::SafeDestruct(losHandler);
	memset(losHandlerMem, 0, sizeof(losHandlerMem));
}


void CLosHandler::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	globalLOS.fill(false);
	replayCheckpointLosStateLoaded = false;

	baseRadarErrorSize = defBaseRadarErrorSize;
	baseRadarErrorMult = defBaseRadarErrorMult;

	los.Init(modInfo.losMipLevel, ILosType::LOS_TYPE_LOS);
	airLos.Init(modInfo.airMipLevel, ILosType::LOS_TYPE_AIRLOS);
	radar.Init(modInfo.radarMipLevel, ILosType::LOS_TYPE_RADAR);
	sonar.Init(modInfo.radarMipLevel, ILosType::LOS_TYPE_SONAR);
	seismic.Init(modInfo.radarMipLevel, ILosType::LOS_TYPE_SEISMIC);
	jammer.Init(modInfo.radarMipLevel, ILosType::LOS_TYPE_JAMMER);
	sonarJammer.Init(modInfo.radarMipLevel, ILosType::LOS_TYPE_SONAR_JAMMER);

	radarErrorSizes.clear();
	radarErrorSizes.resize(teamHandler.ActiveAllyTeams(), defBaseRadarErrorSize);

	losTypes[0] = &los;
	losTypes[1] = &airLos;
	losTypes[2] = &radar;
	losTypes[3] = &sonar;
	losTypes[4] = &seismic;
	losTypes[5] = &jammer;
	losTypes[6] = &sonarJammer;

	eventHandler.AddClient(this);
}

void CLosHandler::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	los.Kill();
	airLos.Kill();
	radar.Kill();
	sonar.Kill();
	seismic.Kill();
	jammer.Kill();
	sonarJammer.Kill();

	/*size_t memUsage = 0;
	for (ILosType* lt: losTypes) {
		memUsage += lt->instances.size() * sizeof(SLosInstance);
		for (SLosInstance& li: lt->instances) {
			memUsage += li.squares.capacity() * sizeof(SLosInstance::RLE);
		}
		memUsage += lt->losMaps.size() * sizeof(CLosMap);
		for (CLosMap& lm: lt->losMaps) {
			memUsage += lm.losmap.capacity() * sizeof(unsigned short);
		}
	}
	LOG_L(L_WARNING, "LosHandler MemUsage: ~%.1fMB", memUsage / (1024.f * 1024.f));*/

	LOG("[LosHandler::%s] raycast instance cache-{hits,misses}={%u,%u}; shared=%.0f%%; cached=%.0f%%",
		__func__, unsigned(ILosType::cacheHits), unsigned(ILosType::cacheFails),
		100.0f * float(ILosType::cacheHits - ILosType::cacheRefs) / (ILosType::cacheHits + ILosType::cacheFails),
		100.0f * float(ILosType::cacheRefs) / (ILosType::cacheHits + ILosType::cacheFails)
	);

	losTypes.fill(nullptr);
	replayCheckpointLosStateLoaded = false;
}

void CLosHandler::ResetLiveMapsForLoad()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (replayCheckpointLosStateLoaded) {
		replayCheckpointLosStateLoaded = false;
		LOG("[ReplayCheckpoint] restored exact LOS live state for load from %u active units",
			static_cast<unsigned int>(unitHandler.GetActiveUnits().size())
		);
		return;
	}

	const ReplayCheckpointLosMapSnapshot loadedMaps = CaptureReplayCheckpointLosMaps(losTypes);

	for (ILosType* lt: losTypes) {
		lt->Kill();
		lt->Init(lt->mipLevel, lt->type);
	}

	for (CUnit* unit: unitHandler.GetActiveUnits()) {
		unit->los.fill(nullptr);
	}

	Update();
	RestoreReplayCheckpointLosMaps(loadedMaps, losTypes);

	LOG("[ReplayCheckpoint] reset LOS live instances for load from %u active units and restored checkpoint LOS maps",
		static_cast<unsigned int>(unitHandler.GetActiveUnits().size())
	);
}

void CLosHandler::SerializeReplayCheckpointState(creg::ISerializer* s)
{
#ifdef USING_CREG
	s->Serialize(ILosType::cacheFails);
	s->Serialize(ILosType::cacheHits);
	s->Serialize(ILosType::cacheRefs);

	for (ILosType* losType: losTypes) {
		assert(losType != nullptr);
		losType->SerializeReplayCheckpoint(s);
	}

	SerializeReplayCheckpointUnitLosLinks(s, losTypes);

	if (!s->IsWriting())
		replayCheckpointLosStateLoaded = true;
#endif
}

uint32_t CLosHandler::GetReplayCheckpointUnitLinkHash() const
{
	uint32_t hash = 0x7c01d005u;

	const auto& activeUnits = unitHandler.GetActiveUnits();
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(activeUnits.size()));

	for (const CUnit* unit: activeUnits) {
		hash = ReplayCheckpointLosHashInt(hash, (unit != nullptr) ? unit->id : -1);

		if (unit == nullptr)
			continue;

		for (size_t typeIdx = 0; typeIdx < losTypes.size(); ++typeIdx) {
			const SLosInstance* instance = unit->los[typeIdx];
			hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(typeIdx));
			hash = ReplayCheckpointLosHashInstancePtr(hash, instance);
			hash = ReplayCheckpointLosHashInt(hash, (instance != nullptr) ? instance->hashNum : 0);
			hash = ReplayCheckpointLosHashInt(hash, (instance != nullptr) ? instance->refCount : 0);
		}
	}

	return hash;
}

uint32_t CLosHandler::GetReplayCheckpointStateHash() const
{
	uint32_t hash = 0x10c05a7eu;

	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(ILosType::cacheFails));
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(ILosType::cacheHits));
	hash = ReplayCheckpointLosHashUInt(hash, static_cast<uint32_t>(ILosType::cacheRefs));

	for (const bool enabled: globalLOS)
		hash = ReplayCheckpointLosHashUInt(hash, enabled ? 1u : 0u);

	hash = ReplayCheckpointLosHashFloat(hash, baseRadarErrorSize);
	hash = ReplayCheckpointLosHashFloat(hash, baseRadarErrorMult);
	hash = ReplayCheckpointLosHashVectorRaw(hash, radarErrorSizes);

	for (const ILosType* losType: losTypes) {
		hash = ReplayCheckpointLosHashUInt(hash, losType != nullptr ? 1u : 0u);
		if (losType != nullptr)
			hash = ReplayCheckpointLosHashUInt(hash, losType->GetReplayCheckpointStateHash());
	}

	hash = ReplayCheckpointLosHashUInt(hash, GetReplayCheckpointUnitLinkHash());
	return hash;
}

void CLosHandler::LogReplayCheckpointStateSignature(const char* label) const
{
	if (configHandler == nullptr)
		return;

	const int debugFrame = configHandler->GetInt("ReplayCheckpointDebugSignatureFrame");
	if (debugFrame < 0 || gs == nullptr || gs->frameNum != debugFrame)
		return;

	uint32_t mapHash = 0x6c05f00du;
	uint32_t queueHash = 0x70510f1fu;
	unsigned int instanceCount = 0;
	unsigned int freeIDCount = 0;
	unsigned int delayedDeleteCount = 0;
	unsigned int delayedTerraCount = 0;
	unsigned int losUpdateCount = 0;
	unsigned int losCacheCount = 0;

	for (const ILosType* losType: losTypes) {
		if (losType == nullptr)
			continue;

		mapHash = ReplayCheckpointLosHashUInt(mapHash, losType->GetReplayCheckpointMapHash());
		queueHash = ReplayCheckpointLosHashUInt(queueHash, losType->GetReplayCheckpointQueueHash());
		instanceCount += static_cast<unsigned int>(losType->instances.size());
		freeIDCount += static_cast<unsigned int>(losType->freeIDs.size());
		delayedDeleteCount += static_cast<unsigned int>(losType->GetReplayCheckpointDelayedDeleteCount());
		delayedTerraCount += static_cast<unsigned int>(losType->GetReplayCheckpointDelayedTerraCount());
		losUpdateCount += static_cast<unsigned int>(losType->GetReplayCheckpointLosUpdateCount());
		losCacheCount += static_cast<unsigned int>(losType->GetReplayCheckpointLosCacheCount());
	}

	LOG("[ReplayCheckpoint][los-sig] %s frame=%d sync=%08x hash=%08x mapHash=%08x queueHash=%08x unitLinkHash=%08x instances=%u freeIDs=%u delayedDelete=%u delayedTerra=%u losUpdate=%u losCache=%u loaded=%u",
		label,
		gs->frameNum,
		CSyncChecker::GetChecksum(),
		GetReplayCheckpointStateHash(),
		mapHash,
		queueHash,
		GetReplayCheckpointUnitLinkHash(),
		instanceCount,
		freeIDCount,
		delayedDeleteCount,
		delayedTerraCount,
		losUpdateCount,
		losCacheCount,
		replayCheckpointLosStateLoaded ? 1u : 0u
	);
}


void CLosHandler::SetGlobalLOS(const int allyTeamId, const bool newState)
{
	RECOIL_DETAILED_TRACY_ZONE;
	globalLOS[allyTeamId] = newState;

	if (globalLOS[allyTeamId])
		readMap->BecomeSpectator(); //update unsynced heightmap
}

void CLosHandler::UnitDestroyed(const CUnit* unit, const CUnit* attacker, int weaponDefID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (ILosType* lt: losTypes) {
		lt->RemoveUnit(const_cast<CUnit*>(unit), true);
	}
}


void CLosHandler::UnitTaken(const CUnit* unit, int oldTeam, int newTeam)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (ILosType* lt: losTypes) {
		lt->RemoveUnit(const_cast<CUnit*>(unit));
	}
}


void CLosHandler::UnitReverseBuilt(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (ILosType* lt: losTypes) {
		lt->RemoveUnit(const_cast<CUnit*>(unit));
	}
}


void CLosHandler::UnitLoaded(const CUnit* unit, const CUnit* transport)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!unit->IsStunned())
		return;

	for (ILosType* lt: losTypes) {
		lt->RemoveUnit(const_cast<CUnit*>(unit));
	}
}


void CLosHandler::Update()
{
	SCOPED_TIMER("Sim::Los");

	const std::vector<CUnit*>& activeUnits = unitHandler.GetActiveUnits();

	#if (USE_STAGGERED_UPDATES == 1)
	const size_t losBatchRate = UNIT_SLOWUPDATE_RATE;
	const size_t losBatchSize = std::max(size_t(1), activeUnits.size() / losBatchRate);
	const size_t losBatchMult = gs->frameNum % losBatchRate;
	const size_t minUnitIndex = losBatchSize * losBatchMult;
	const size_t maxUnitIndex = minUnitIndex + losBatchSize + (activeUnits.size() % losBatchRate) * (losBatchMult == (losBatchRate - 1));
	#endif

	for_mt(0, losTypes.size(), [&](const int idx) {
		ILosType* lt = losTypes[idx];

		#if (USE_STAGGERED_UPDATES == 1)
		// staggered
		for (size_t n = 0; n < activeUnits.size(); n++) {
			lt->UpdateUnit(activeUnits[n], n < minUnitIndex || n >= maxUnitIndex);
		}
		#else
		// all at once
		{
			ZoneScopedN("Sim::Los::UpdateLosTypeMT");
			for (CUnit* u : activeUnits) {
				lt->UpdateUnit(u, false);
			}
		}
		#endif

		lt->Update();
	});
}


void CLosHandler::UpdateHeightMapSynced(SRectangle rect)
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (ILosType* lt: losTypes) {
		ZoneScopedN("LosHandler::UpdateHeightMapSynced");
		lt->UpdateHeightMapSynced(rect);
	}
}


bool CLosHandler::InLos(const CUnit* unit, int allyTeam) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// NOTE: units are treated differently than world objects in two ways:
	//   1. they can be cloaked (has to be checked BEFORE all other cases)
	//   2. when underwater, they are only considered to be in LOS if they
	//      are also in radar ("sonar") coverage if requireSonarUnderWater
	//      is enabled --> underwater units can NOT BE SEEN AT ALL without
	//      active radar!
	if (modInfo.alwaysVisibleOverridesCloaked) {
		if (unit->alwaysVisible)
			return true;
		if (unit->isCloaked && unit->allyteam != allyTeam)
			return false;
	} else {
		if (unit->isCloaked && unit->allyteam != allyTeam)
			return false;
		if (unit->alwaysVisible)
			return true;
	}

	// isCloaked always overrides globalLOS
	if (globalLOS[allyTeam])
		return true;

	if (unit->useAirLos)
		return (InAirLos(unit->pos, allyTeam) || InAirLos(unit->pos + unit->speed, allyTeam));

	if (modInfo.requireSonarUnderWater) {
		if (unit->IsUnderWater() && !InRadar(unit, allyTeam)) {
			return false;
		}
	}

	return (InLos(unit->pos, allyTeam) || InLos(unit->pos + unit->speed, allyTeam));
}


bool CLosHandler::InAirLos(const CUnit* unit, int allyTeam) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// NOTE: units are treated differently than world objects in two ways:
	//   1. they can be cloaked (has to be checked BEFORE all other cases)
	//   2. when underwater, they are only considered to be in LOS if they
	//      are also in radar ("sonar") coverage if requireSonarUnderWater
	//      is enabled --> underwater units can NOT BE SEEN AT ALL without
	//      active radar!
	if (modInfo.alwaysVisibleOverridesCloaked) {
		if (unit->alwaysVisible)
			return true;
		if (unit->isCloaked && unit->allyteam != allyTeam)
			return false;
	} else {
		if (unit->isCloaked && unit->allyteam != allyTeam)
			return false;
		if (unit->alwaysVisible)
			return true;
	}

	// isCloaked always overrides globalLOS
	if (globalLOS[allyTeam])
		return true;

	if (modInfo.requireSonarUnderWater) {
		if (unit->IsUnderWater() && !InRadar(unit, allyTeam))
			return false;
	}

	return airLos.InSight(unit->pos, allyTeam);
}


bool CLosHandler::InRadar(const float3 pos, int allyTeam) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// position is underwater, only sonar can see it
	// note: only check jammers when we have a common jammer map, else jammers only apply to objects!
	if (pos.y < 0.0f)
		return (sonar.InSight(pos, allyTeam) && !(!modInfo.separateJammers && sonarJammer.InSight(pos, 0)));

	return (radar.InSight(pos, allyTeam) && !(!modInfo.separateJammers && jammer.InSight(pos, 0)));
}


bool CLosHandler::InRadar(const CUnit* unit, int allyTeam) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// unit is discoverable by sonar
	if (unit->IsInWater()) {
		if ((!unit->sonarStealth || unit->beingBuilt) &&
		    sonar.InSight(unit->pos, allyTeam) &&
		    !InJammer(unit, allyTeam))
			return true;
	}

	// unit is completely submerged, only sonar can see it
	if (unit->IsUnderWater())
		return false;

	// radar stealth
	if (unit->stealth && !unit->beingBuilt)
		return false;

	return (radar.InSight(unit->pos, allyTeam) && !InJammer(unit, allyTeam));
}


bool CLosHandler::InJammer(const float3 pos, int allyTeam) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const int jammerAlly = modInfo.separateJammers ? allyTeam : 0;

	if (pos.y < 0.0f)
		return sonarJammer.InSight(pos, jammerAlly);

	return jammer.InSight(pos, jammerAlly);
}


bool CLosHandler::InJammer(const CUnit* unit, int allyTeam) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (allyTeam == unit->allyteam)
		return false;

	//TODO handle ingame alliances

	const int jammerAlly = modInfo.separateJammers ? unit->allyteam : 0;

	if (unit->IsUnderWater()) {
		return sonarJammer.InSight(unit->pos, jammerAlly);
	}
	return jammer.InSight(unit->pos, jammerAlly);
}
