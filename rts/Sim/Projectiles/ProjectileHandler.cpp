/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <algorithm>
#include <cstring>

#include "Projectile.h"
#include "ProjectileHandler.h"
#include "ProjectileMemPool.h"
#include "Game/GlobalUnsynced.h"
#include "Game/TraceRay.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GroundFlash.h"
#include "Rendering/Models/3DModelPiece.hpp"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/TeamHandler.h"
#include "Rendering/Env/Particles/Classes/NanoProjectile.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDef.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
#include "System/Cpp11Compat.hpp"
#include "System/SpringHash.h"
#include "System/SpringMath.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"

#include "System/Misc/TracyDefs.h"


// reserve 5% of maxNanoParticles for important stuff such as capture and reclaim other teams' units
#define NORMAL_NANO_PRIO 0.95f
#define HIGH_NANO_PRIO 1.0f


CONFIG(int, MaxParticles).defaultValue(10000).headlessValue(0).minimumValue(0);
CONFIG(int, MaxNanoParticles).defaultValue(2000).headlessValue(0).minimumValue(0);


CR_BIND(CProjectileHandler, )
CR_REG_METADATA(CProjectileHandler, (
	CR_MEMBER(projectiles),
	CR_MEMBER_UN(flyingPieces),
	CR_MEMBER_UN(groundFlashes),
	CR_MEMBER_UN(resortFlyingPieces),

	CR_MEMBER(maxParticles),
	CR_MEMBER(maxNanoParticles),
	CR_MEMBER(currentNanoParticles),
	CR_MEMBER_UN(frameCurrentParticles),
	CR_MEMBER_UN(frameProjectileCounts)
))



// note: stores all ExpGenSpawnable types, not just projectiles
ProjMemPool projMemPool;

CProjectileHandler projectileHandler;



void CProjectileHandler::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	currentNanoParticles = 0;
	frameCurrentParticles = 0;
	frameProjectileCounts[false] = 0;
	frameProjectileCounts[ true] = 0;

	resortFlyingPieces.fill(false);

	maxParticles     = configHandler->GetInt("MaxParticles");
	maxNanoParticles = configHandler->GetInt("MaxNanoParticles");

	projMemPool.clear();
	projMemPool.reserve(1024);

	for (int modelType = 0; modelType < MODELTYPE_CNT; ++modelType) {
		flyingPieces[modelType].clear();
		flyingPieces[modelType].reserve(1000);
	}

	projectiles[true ].SeedFreeKeys(0, 1 << 14, true); //seed only synced free ids.
	projectiles[false].reserve(static_cast<size_t>(maxParticles) * 2);

	CExpGenSpawnable::InitSpawnables();

	// register ConfigNotify()
	configHandler->NotifyOnChange(this, {"MaxParticles", "MaxNanoParticles"});
}

void CProjectileHandler::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	configHandler->RemoveObserver(this);

	{
		// synced first, to avoid callback crashes
		for (CProjectile* p: projectiles[true])
			projMemPool.free(p);

		projectiles[true].clear();
	}

	{
		for (CProjectile* p: projectiles[false])
			projMemPool.free(p);

		projectiles[false].clear();
	}

	{
		for (CGroundFlash* gf: groundFlashes)
			projMemPool.free(gf);

		groundFlashes.clear();
	}

	{
		for (auto& fpc: flyingPieces) {
			fpc.clear();
		}
	}

	CCollisionHandler::PrintStats();
}


void CProjectileHandler::ConfigNotify(const std::string& key, const std::string& value)
{
	RECOIL_DETAILED_TRACY_ZONE;
	maxParticles     = configHandler->GetInt("MaxParticles");
	maxNanoParticles = configHandler->GetInt("MaxNanoParticles");

	projectiles[false].reserve(static_cast<size_t>(maxParticles) * 2);
}


static void MAPPOS_SANITY_CHECK(const float3 v)
{
	RECOIL_DETAILED_TRACY_ZONE;
	v.AssertNaNs();
	assert(v.x >= -(float3::maxxpos * 16.0f));
	assert(v.x <=  (float3::maxxpos * 16.0f));
	assert(v.z >= -(float3::maxzpos * 16.0f));
	assert(v.z <=  (float3::maxzpos * 16.0f));
	assert(v.y >= -MAX_PROJECTILE_HEIGHT);
	assert(v.y <=  MAX_PROJECTILE_HEIGHT);
}

static bool ReplayCheckpointDebugProjectileFrame()
{
	const int debugFrame = configHandler->GetInt("ReplayCheckpointDebugSignatureFrame");
	return (debugFrame >= 0 && gs->frameNum == debugFrame);
}

static uint32_t ReplayCheckpointProjectileHashInt(uint32_t hash, int value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

static uint32_t ReplayCheckpointProjectileHashUInt(uint32_t hash, uint32_t value)
{
	return spring::LiteHash(&value, sizeof(value), hash);
}

size_t CProjectileHandler::GetReplayCheckpointFreeKeyCount(bool synced) const
{
	return projectiles[synced].GetFreeKeys().size();
}

uint32_t CProjectileHandler::GetReplayCheckpointFreeKeyHash(bool synced) const
{
	uint32_t hash = 0x6d2b79f5u;

	for (const int key: projectiles[synced].GetFreeKeys())
		hash = ReplayCheckpointProjectileHashInt(hash, key);

	return hash;
}

uint32_t CProjectileHandler::GetReplayCheckpointActiveKeyHash(bool synced) const
{
	uint32_t hash = 0xf00d1234u;

	for (const int key: projectiles[synced].GetKeys())
		hash = ReplayCheckpointProjectileHashInt(hash, key);

	return hash;
}

static uint32_t ReplayCheckpointProjectileHashFloat(uint32_t hash, float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return ReplayCheckpointProjectileHashUInt(hash, bits);
}

static uint32_t ReplayCheckpointProjectileHashFloat3(uint32_t hash, const float3& value)
{
	hash = ReplayCheckpointProjectileHashFloat(hash, value.x);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.y);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.z);
	return hash;
}

static uint32_t ReplayCheckpointProjectileHashFloat4(uint32_t hash, const float4& value)
{
	hash = ReplayCheckpointProjectileHashFloat(hash, value.x);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.y);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.z);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.w);
	return hash;
}

static uint32_t ReplayCheckpointProjectileHashMatrix(uint32_t hash, const CMatrix44f& value)
{
	for (int i = 0; i < 16; ++i) {
		hash = ReplayCheckpointProjectileHashFloat(hash, value[i]);
	}

	return hash;
}

static uint32_t ReplayCheckpointProjectileHashTransform(uint32_t hash, const Transform& value)
{
	hash = ReplayCheckpointProjectileHashFloat(hash, value.r.x);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.r.y);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.r.z);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.r.r);
	hash = ReplayCheckpointProjectileHashFloat3(hash, value.t);
	hash = ReplayCheckpointProjectileHashFloat(hash, value.s);
	return hash;
}

static uint32_t ReplayCheckpointProjectileHashCollisionVolume(uint32_t hash, const CollisionVolume* volume)
{
	if (volume == nullptr)
		return ReplayCheckpointProjectileHashInt(hash, -1);

	hash = ReplayCheckpointProjectileHashInt(hash, volume->GetVolumeType());
	hash = ReplayCheckpointProjectileHashInt(hash, volume->GetPrimaryAxis());
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(volume->IgnoreHits()));
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(volume->UseContHitTest()));
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(volume->DefaultToPieceTree()));
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(volume->DefaultToFootPrint()));
	hash = ReplayCheckpointProjectileHashFloat3(hash, volume->GetScales());
	hash = ReplayCheckpointProjectileHashFloat3(hash, volume->GetOffsets());
	hash = ReplayCheckpointProjectileHashFloat(hash, volume->GetBoundingRadius());
	return hash;
}

static uint32_t ReplayCheckpointProjectileHashLocalModelPiece(uint32_t hash, const LocalModelPiece& piece, bool resolved)
{
	hash = ReplayCheckpointProjectileHashInt(hash, piece.GetLModelPieceIndex());
	hash = ReplayCheckpointProjectileHashInt(hash, piece.GetScriptPieceIndex());
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(piece.GetScriptVisible()));
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(piece.GetDirty()));
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(piece.blockScriptAnims));
	hash = ReplayCheckpointProjectileHashFloat3(hash, piece.GetPosition());
	hash = ReplayCheckpointProjectileHashFloat3(hash, piece.GetRotation());
	hash = ReplayCheckpointProjectileHashFloat(hash, piece.GetScaling());
	hash = ReplayCheckpointProjectileHashFloat3(hash, piece.GetDirection());
	hash = ReplayCheckpointProjectileHashTransform(hash, piece.GetModelSpaceTransformRaw());
	hash = ReplayCheckpointProjectileHashCollisionVolume(hash, piece.GetCollisionVolume());

	if (resolved) {
		hash = ReplayCheckpointProjectileHashTransform(hash, piece.GetModelSpaceTransform());
		hash = ReplayCheckpointProjectileHashMatrix(hash, piece.GetModelSpaceMatrix());
	}

	return hash;
}

static uint32_t ReplayCheckpointProjectileHashLocalModel(const CUnit* unit, bool resolved)
{
	uint32_t hash = 0x514c4d31u;

	if (unit == nullptr)
		return hash;

	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(unit->localModel.pieces.size()));
	hash = ReplayCheckpointProjectileHashInt(hash, static_cast<int>(unit->localModel.GetBoundariesNeedsRecalc()));
	hash = ReplayCheckpointProjectileHashCollisionVolume(hash, unit->localModel.GetBoundingVolume());

	for (const LocalModelPiece& piece: unit->localModel.pieces) {
		hash = ReplayCheckpointProjectileHashLocalModelPiece(hash, piece, resolved);
	}

	return hash;
}

static void ReplayCheckpointLogProjectileEvent(const char* label, const CProjectile* p)
{
	if (!ReplayCheckpointDebugProjectileFrame() || p == nullptr || !p->synced)
		return;

	const int debugProjectileID = configHandler->GetInt("ReplayCheckpointDebugDamageProjectileID");
	if (debugProjectileID >= 0 && p->id != debugProjectileID)
		return;

	const auto* wp = dynamic_cast<const CWeaponProjectile*>(p);
	const char* className = "unknown";

#ifdef USING_CREG
	if (p->GetClass() != nullptr)
		className = p->GetClass()->name;
#endif

	LOG("[ReplayCheckpoint][proj] %s frame=%d id=%d class=%s weapon=%d piece=%d type=%u create=%d delete=%d checkCol=%d pos=(%f,%f,%f) speed=(%f,%f,%f,%f) pre=(%f,%f,%f) owner=%d team=%d allyteam=%d ttl=%d targetPos=(%f,%f,%f) startPos=(%f,%f,%f) bounced=%d",
		label,
		gs->frameNum,
		p->id,
		className,
		p->weapon,
		p->piece,
		p->GetProjectileType(),
		p->createMe,
		p->deleteMe,
		p->checkCol,
		p->pos.x, p->pos.y, p->pos.z,
		p->speed.x, p->speed.y, p->speed.z, p->speed.w,
		p->preFrameTra.t.x, p->preFrameTra.t.y, p->preFrameTra.t.z,
		p->GetOwnerID(),
		p->GetTeamID(),
		p->GetAllyteamID(),
		(wp != nullptr) ? wp->GetTimeToLive() : -1,
		(wp != nullptr) ? wp->GetTargetPos().x : 0.0f,
		(wp != nullptr) ? wp->GetTargetPos().y : 0.0f,
		(wp != nullptr) ? wp->GetTargetPos().z : 0.0f,
		(wp != nullptr) ? wp->GetStartPos().x : 0.0f,
		(wp != nullptr) ? wp->GetStartPos().y : 0.0f,
		(wp != nullptr) ? wp->GetStartPos().z : 0.0f,
		(wp != nullptr) ? wp->HasScheduledBounce() : false);
}

static bool ReplayCheckpointShouldLogProjectileCollisionTrace(const CProjectile* p)
{
	if (!ReplayCheckpointDebugProjectileFrame() || p == nullptr || !p->synced)
		return false;

	const int debugProjectileID = configHandler->GetInt("ReplayCheckpointDebugDamageProjectileID");
	return (debugProjectileID >= 0 && p->id == debugProjectileID);
}

static void ReplayCheckpointLogProjectileLocalModelState(const char* label, const CProjectile* p, const CUnit* unit, bool resolved)
{
	if (!ReplayCheckpointShouldLogProjectileCollisionTrace(p) || unit == nullptr)
		return;

	const int debugUnitID = configHandler->GetInt("ReplayCheckpointDebugTargetQueryUnit");
	if (debugUnitID < 0 || unit->id != debugUnitID)
		return;

	const CollisionVolume* boundingVolume = unit->localModel.GetBoundingVolume();
	const uint32_t hash = ReplayCheckpointProjectileHashLocalModel(unit, resolved);

	LOG("[ReplayCheckpoint][unit-localmodel] %s frame=%d projectile=%d unit=%d pieces=%u resolved=%d hash=%08x boundariesNeed=%d bvType=%d bvAxis=%d bvScales=<%f,%f,%f> bvOffsets=<%f,%f,%f> bvRadius=%f",
		label,
		gs->frameNum,
		p->id,
		unit->id,
		static_cast<unsigned int>(unit->localModel.pieces.size()),
		static_cast<int>(resolved),
		hash,
		static_cast<int>(unit->localModel.GetBoundariesNeedsRecalc()),
		boundingVolume->GetVolumeType(),
		boundingVolume->GetPrimaryAxis(),
		boundingVolume->GetScales().x, boundingVolume->GetScales().y, boundingVolume->GetScales().z,
		boundingVolume->GetOffsets().x, boundingVolume->GetOffsets().y, boundingVolume->GetOffsets().z,
		boundingVolume->GetBoundingRadius()
	);

	for (const LocalModelPiece& piece: unit->localModel.pieces) {
		const CollisionVolume* pieceVolume = piece.GetCollisionVolume();
		if (!piece.GetScriptVisible() || pieceVolume->IgnoreHits())
			continue;

		const Transform& rawTra = piece.GetModelSpaceTransformRaw();
		const Transform& modelTra = resolved ? piece.GetModelSpaceTransform() : rawTra;
		const CMatrix44f* modelMat = resolved ? &piece.GetModelSpaceMatrix() : nullptr;
		const char* pieceName = (piece.original != nullptr) ? piece.original->name.c_str() : "<null>";

		LOG("[ReplayCheckpoint][unit-localmodel-piece] %s frame=%d projectile=%d unit=%d piece=%d scriptPiece=%d name=%s visible=%d dirty=%d block=%d pos=<%f,%f,%f> rot=<%f,%f,%f> scale=%f rawT=<%f,%f,%f> rawQ=<%f,%f,%f,%f> rawS=%f modelT=<%f,%f,%f> modelQ=<%f,%f,%f,%f> modelS=%f matT=<%f,%f,%f> cvType=%d cvAxis=%d cvScales=<%f,%f,%f> cvOffsets=<%f,%f,%f> cvRadius=%f",
			label,
			gs->frameNum,
			p->id,
			unit->id,
			piece.GetLModelPieceIndex(),
			piece.GetScriptPieceIndex(),
			pieceName,
			static_cast<int>(piece.GetScriptVisible()),
			static_cast<int>(piece.GetDirty()),
			static_cast<int>(piece.blockScriptAnims),
			piece.GetPosition().x, piece.GetPosition().y, piece.GetPosition().z,
			piece.GetRotation().x, piece.GetRotation().y, piece.GetRotation().z,
			piece.GetScaling(),
			rawTra.t.x, rawTra.t.y, rawTra.t.z,
			rawTra.r.x, rawTra.r.y, rawTra.r.z, rawTra.r.r,
			rawTra.s,
			modelTra.t.x, modelTra.t.y, modelTra.t.z,
			modelTra.r.x, modelTra.r.y, modelTra.r.z, modelTra.r.r,
			modelTra.s,
			(modelMat != nullptr) ? (*modelMat)[12] : 0.0f,
			(modelMat != nullptr) ? (*modelMat)[13] : 0.0f,
			(modelMat != nullptr) ? (*modelMat)[14] : 0.0f,
			pieceVolume->GetVolumeType(),
			pieceVolume->GetPrimaryAxis(),
			pieceVolume->GetScales().x, pieceVolume->GetScales().y, pieceVolume->GetScales().z,
			pieceVolume->GetOffsets().x, pieceVolume->GetOffsets().y, pieceVolume->GetOffsets().z,
			pieceVolume->GetBoundingRadius()
		);
	}
}

static void ReplayCheckpointLogProjectileSignature(const char* label)
{
	if (!ReplayCheckpointDebugProjectileFrame())
		return;

	uint32_t hash = 0x92c6a35du;
	uint32_t count = 0;

	for (const CProjectile* p : projectileHandler.GetActiveProjectiles(true)) {
		const auto* wp = dynamic_cast<const CWeaponProjectile*>(p);

		hash = ReplayCheckpointProjectileHashInt(hash, p->id);
		hash = ReplayCheckpointProjectileHashInt(hash, p->weapon);
		hash = ReplayCheckpointProjectileHashInt(hash, p->piece);
		hash = ReplayCheckpointProjectileHashInt(hash, p->createMe);
		hash = ReplayCheckpointProjectileHashInt(hash, p->deleteMe);
		hash = ReplayCheckpointProjectileHashInt(hash, p->checkCol);
		hash = ReplayCheckpointProjectileHashUInt(hash, p->GetProjectileType());
		hash = ReplayCheckpointProjectileHashUInt(hash, p->GetCollisionFlags());
		hash = ReplayCheckpointProjectileHashFloat3(hash, p->pos);
		hash = ReplayCheckpointProjectileHashFloat4(hash, p->speed);
		hash = ReplayCheckpointProjectileHashFloat3(hash, p->preFrameTra.t);
		hash = ReplayCheckpointProjectileHashInt(hash, p->GetOwnerID());
		hash = ReplayCheckpointProjectileHashInt(hash, p->GetTeamID());
		hash = ReplayCheckpointProjectileHashInt(hash, p->GetAllyteamID());

		if (wp != nullptr) {
			hash = ReplayCheckpointProjectileHashInt(hash, wp->GetTimeToLive());
			hash = ReplayCheckpointProjectileHashFloat3(hash, wp->GetTargetPos());
			hash = ReplayCheckpointProjectileHashFloat3(hash, wp->GetStartPos());
			hash = ReplayCheckpointProjectileHashInt(hash, wp->HasScheduledBounce());
		}

		++count;
	}

	LOG("[ReplayCheckpoint][proj-sig] %s frame=%d syncedProjectiles=%u hash=%08x activeKeyHash=%08x freeKeys=%u freeKeyHash=%08x",
		label,
		gs->frameNum,
		count,
		hash,
		projectileHandler.GetReplayCheckpointActiveKeyHash(true),
		static_cast<unsigned int>(projectileHandler.GetReplayCheckpointFreeKeyCount(true)),
		projectileHandler.GetReplayCheckpointFreeKeyHash(true));
}

template<bool synced>
void CProjectileHandler::UpdateProjectilesImpl()
{
	SCOPED_TIMER("Sim::Projectiles::Update");

	auto& pc = projectiles[synced];
	// WARNING:
	//   we can't use iterators here because ProjectileCreated
	//   and ProjectileDestroyed events may add new projectiles
	//   to the container!
	for (size_t i = 0; i < pc.size(); /*no-op*/) {
		CProjectile* p = pc[i];

		assert(p != nullptr);
		assert(p->synced == synced);
#ifdef USING_CREG
		assert(p->synced == !!(p->GetClass()->flags & creg::CF_Synced));
#endif

		// (delayed) creation for projectiles added after CheckCollisions()
		if (p->createMe) {
			ReplayCheckpointLogProjectileEvent("create-before-update", p);
			CreateProjectile(p);
		}

		// deletion (FIXME: move outside of loop)
		if (p->deleteMe) {
			ReplayCheckpointLogProjectileEvent("destroy-before-update", p);
			DestroyProjectile(p);
			continue;
		}

		// neither
		++i;
	}

	// WARNING: same as above but for p->Update()
	if constexpr (synced) {

		SCOPED_TIMER("Sim::Projectiles::UpdateSyncedST");
		for (size_t i = 0; i < pc.size(); ++i) {
			CProjectile* p = pc[i];
			assert(p != nullptr);

			MAPPOS_SANITY_CHECK(p->pos);
			p->PreUpdate();
			p->Update();
			if (p->deleteMe)
				ReplayCheckpointLogProjectileEvent("marked-delete-after-update", p);
			quadField.MovedProjectile(p);

			MAPPOS_SANITY_CHECK(p->pos);
		}
	}
	else {
		SCOPED_TIMER("Sim::Projectiles::UpdateUnsyncedMT");
		for_mt_chunk(0, pc.size(), [&pc](int i) {
			CProjectile* p = pc[i];
			assert(p != nullptr);

			MAPPOS_SANITY_CHECK(p->pos);
			p->PreUpdate();
			p->Update();
			MAPPOS_SANITY_CHECK(p->pos);
		});
	}
}


template<class T>
static void UPDATE_PTR_CONTAINER(T& cont) {
	if (cont.empty())
		return;

#ifndef NDEBUG
	const size_t origSize = cont.size();
#endif
	size_t size = cont.size();

	for (size_t i = 0; i < size; /*no-op*/) {
		CGroundFlash*& gf = cont[i];

		if (!gf->Update()) {
			projMemPool.free(gf);
			gf = cont[size -= 1];
			continue;
		}

		++i;
	}

	// WARNING:
	//   check if the vector was enlarged while iterating, in
	//   which case we will have missed updating newest items
	assert(cont.size() == origSize);

	cont.erase(cont.begin() + size, cont.end());
}

template<class T>
static void UPDATE_REF_CONTAINER(T& cont) {
	if (cont.empty())
		return;

#ifndef NDEBUG
	const size_t origSize = cont.size();
#endif
	size_t size = cont.size();

	for (size_t i = 0; i < size; /*no-op*/) {
		auto& p = cont[i];

		if (!p.Update()) {
			p = std::move(cont[size -= 1]);
			continue;
		}

		++i;
	}

	// WARNING: see UPDATE_PTR_CONTAINER
	assert(cont.size() == origSize);

	cont.erase(cont.begin() + size, cont.end());
}



void CProjectileHandler::CreateProjectile(CProjectile* p)
{
	RECOIL_DETAILED_TRACY_ZONE;
	p->createMe = false;

	if (p->synced || PH_UNSYNCED_PROJECTILE_EVENTS == 1)
		eventHandler.ProjectileCreated(p, p->GetAllyteamID());

	eventHandler.RenderProjectileCreated(p);
}

void CProjectileHandler::DestroyProjectile(CProjectile* p)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(!p->createMe);

	ReplayCheckpointLogProjectileEvent("destroy", p);

	eventHandler.RenderProjectileDestroyed(p);

	if (p->synced) {
		//modelUniformsStorage.DelObject(p);

		eventHandler.ProjectileDestroyed(p, p->GetAllyteamID());

		projectiles[true].Del(p->id);

		ASSERT_SYNCED(p->pos);
		ASSERT_SYNCED(p->id);
	} else {
	#if (PH_UNSYNCED_PROJECTILE_EVENTS == 1)
		eventHandler.ProjectileDestroyed(p, p->GetAllyteamID());
	#endif
		projectiles[false].Del(p->id);
	}

	projMemPool.free(p);
}

uint32_t CProjectileHandler::UnsyncedRandInt(uint32_t N) { return guRNG.NextInt(N); }
uint32_t CProjectileHandler::SyncedRandInt  (uint32_t N) { return gsRNG.NextInt(N); }

void CProjectileHandler::Update()
{
	{
		SCOPED_TIMER("Sim::Projectiles");
		ReplayCheckpointLogProjectileSignature("begin");

		// check if any projectiles have collided since the previous update
		CheckCollisions();
		ReplayCheckpointLogProjectileSignature("after-collisions");
		UpdateProjectiles();
		ReplayCheckpointLogProjectileSignature("after-update-projectiles");

		UPDATE_PTR_CONTAINER(groundFlashes);

		// flying pieces; sort these every now and then
		for (int modelType = 0; modelType < MODELTYPE_CNT; ++modelType) {
			auto& fpc = flyingPieces[modelType];

			UPDATE_REF_CONTAINER(fpc);

			if (resortFlyingPieces[modelType]) {
				std::stable_sort(fpc.begin(), fpc.end());
			}
		}
	}

	// precache part of particles count calculation that else becomes very heavy
	{
		ZoneScopedNC("ProjectileHandler::CountParticles", tracy::Color::Goldenrod);
		frameCurrentParticles = 0;

		for (const CProjectile* p : projectiles[true]) {
			frameCurrentParticles += p->GetProjectilesCount();
		}
		for (const CProjectile* p : projectiles[false]) {
			frameCurrentParticles += p->GetProjectilesCount();
		}

		frameProjectileCounts[true] = projectiles[true].size();
		frameProjectileCounts[false] = projectiles[false].size();
	}
}

void CProjectileHandler::AddProjectile(CProjectile* p)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// already initialized?
	assert(p->id < 0);
	assert(p->createMe);

	const size_t syncedFreeKeyCountBefore = p->synced ? GetReplayCheckpointFreeKeyCount(true) : 0;
	const uint32_t syncedFreeKeyHashBefore = p->synced ? GetReplayCheckpointFreeKeyHash(true) : 0u;

	if (p->synced)
		p->id = static_cast<int>(projectiles[true ].Add(p, rngFuncs[true]));
	else
		p->id = static_cast<int>(projectiles[false].Add(p)); //don't bother with shuffling unsynced ids 

	if (p->synced) {
		ASSERT_SYNCED(freeIDs.size());
		ASSERT_SYNCED(p->id);

		if (ReplayCheckpointDebugProjectileFrame()) {
			LOG("[ReplayCheckpoint][proj-add] frame=%d id=%d freeBefore=%u freeHashBefore=%08x freeAfter=%u freeHashAfter=%08x activeKeyHash=%08x rng=%llu",
				gs->frameNum,
				p->id,
				static_cast<unsigned int>(syncedFreeKeyCountBefore),
				syncedFreeKeyHashBefore,
				static_cast<unsigned int>(GetReplayCheckpointFreeKeyCount(true)),
				GetReplayCheckpointFreeKeyHash(true),
				GetReplayCheckpointActiveKeyHash(true),
				static_cast<unsigned long long>(gsRNG.GetGenState()));
			ReplayCheckpointLogProjectileEvent("add", p);
		}
	}

	CreateProjectile(p);
}




static bool CheckProjectileCollisionFlags(const CProjectile* p, const CUnit* u)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const unsigned int collFlags = p->GetCollisionFlags() * p->weapon;

	// only weapon-projectiles can have non-zero flags
	if (collFlags == 0)
		return true;

	// disregard everything else when this bit is set
	// (ground and feature flags are tested elsewhere)
	if ((collFlags & Collision::NONONTARGETS) != 0)
		return (static_cast<const CWeaponProjectile*>(p)->GetTargetObject() == u);

	if ((collFlags & Collision::NOCLOAKED) != 0 && u->IsCloaked())
		return false;
	if ((collFlags & Collision::NONEUTRALS) != 0 && u->IsNeutral())
		return false;

	if ((collFlags & Collision::NOFIREBASES) != 0) {
		const CUnit* owner = p->owner();
		const CUnit* trans = (owner != nullptr)? owner->GetTransporter(): nullptr;

		// check if the unit being collided with is occupied by p's owner
		if (u == trans && trans->unitDef->isFirePlatform)
			return false;
	}

	if (teamHandler.IsValidAllyTeam(p->GetAllyteamID())) {
		const bool noFriendsBit = ((collFlags & Collision::NOFRIENDLIES) != 0);
		const bool noEnemiesBit = ((collFlags & Collision::NOENEMIES   ) != 0);
		const bool friendlyFire = teamHandler.AlliedAllyTeams(p->GetAllyteamID(), u->allyteam);

		if (noFriendsBit && friendlyFire)
			return false;
		if (noEnemiesBit && !friendlyFire)
			return false;
	}

	return true;
}


void CProjectileHandler::CheckUnitCollisions(
	CProjectile* p,
	std::vector<CUnit*>& tempUnits,
	const float3 ppos0,
	const float3 ppos1
) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (!p->checkCol)
		return;

	CollisionQuery cq;
	const bool replayCheckpointTrace = ReplayCheckpointShouldLogProjectileCollisionTrace(p);
	const auto* wp = replayCheckpointTrace ? dynamic_cast<const CWeaponProjectile*>(p) : nullptr;
	const int replayCheckpointDebugUnitID = ReplayCheckpointDebugProjectileFrame()
		? configHandler->GetInt("ReplayCheckpointDebugTargetQueryUnit")
		: -1;

	for (size_t unitIndex = 0; unitIndex < tempUnits.size(); ++unitIndex) {
		CUnit* unit = tempUnits[unitIndex];
		assert(unit != nullptr);

		const bool owner = (unit == p->owner());
		const bool collidable = unit->HasCollidableStateBit(CSolidObject::CSTATE_BIT_PROJECTILES);
		bool flagsOk = false;
		bool hit = false;

		// if this unit fired this projectile, always ignore
		if (owner) {
			if (replayCheckpointTrace) {
				LOG("[ReplayCheckpoint][proj-collision] candidate frame=%d projectile=%d index=%u unit=%d owner=1 collidable=%d flags=0 hit=0 target=%d pos=<%f,%f,%f> p0=<%f,%f,%f> p1=<%f,%f,%f>",
					gs->frameNum,
					p->id,
					static_cast<unsigned int>(unitIndex),
					unit->id,
					static_cast<int>(collidable),
					(wp != nullptr && wp->GetTargetObject() != nullptr) ? wp->GetTargetObject()->id : -1,
					unit->pos.x, unit->pos.y, unit->pos.z,
					ppos0.x, ppos0.y, ppos0.z,
					ppos1.x, ppos1.y, ppos1.z
				);
			}
			continue;
		}

		if (!collidable) {
			if (replayCheckpointTrace) {
				LOG("[ReplayCheckpoint][proj-collision] candidate frame=%d projectile=%d index=%u unit=%d owner=0 collidable=0 flags=0 hit=0 target=%d pos=<%f,%f,%f> p0=<%f,%f,%f> p1=<%f,%f,%f>",
					gs->frameNum,
					p->id,
					static_cast<unsigned int>(unitIndex),
					unit->id,
					(wp != nullptr && wp->GetTargetObject() != nullptr) ? wp->GetTargetObject()->id : -1,
					unit->pos.x, unit->pos.y, unit->pos.z,
					ppos0.x, ppos0.y, ppos0.z,
					ppos1.x, ppos1.y, ppos1.z
				);
			}
			continue;
		}

		flagsOk = CheckProjectileCollisionFlags(p, unit);
		if (!flagsOk) {
			if (replayCheckpointTrace) {
				LOG("[ReplayCheckpoint][proj-collision] candidate frame=%d projectile=%d index=%u unit=%d owner=0 collidable=1 flags=0 hit=0 target=%d pos=<%f,%f,%f> p0=<%f,%f,%f> p1=<%f,%f,%f>",
					gs->frameNum,
					p->id,
					static_cast<unsigned int>(unitIndex),
					unit->id,
					(wp != nullptr && wp->GetTargetObject() != nullptr) ? wp->GetTargetObject()->id : -1,
					unit->pos.x, unit->pos.y, unit->pos.z,
					ppos0.x, ppos0.y, ppos0.z,
					ppos1.x, ppos1.y, ppos1.z
				);
			}
			continue;
		}

		ReplayCheckpointLogProjectileLocalModelState("pre-detect", p, unit, false);
		hit = CCollisionHandler::DetectHit(unit, unit->GetTransformMatrix(true), ppos0, ppos1, &cq);
		ReplayCheckpointLogProjectileLocalModelState("post-detect", p, unit, true);
		if (replayCheckpointTrace || unit->id == replayCheckpointDebugUnitID) {
			LOG("[ReplayCheckpoint][proj-collision] candidate frame=%d projectile=%d index=%u unit=%d owner=0 collidable=1 flags=1 hit=%d target=%d pos=<%f,%f,%f> p0=<%f,%f,%f> p1=<%f,%f,%f>%s",
				gs->frameNum,
				p->id,
				static_cast<unsigned int>(unitIndex),
				unit->id,
				static_cast<int>(hit),
				(wp != nullptr && wp->GetTargetObject() != nullptr) ? wp->GetTargetObject()->id : -1,
				unit->pos.x, unit->pos.y, unit->pos.z,
				ppos0.x, ppos0.y, ppos0.z,
				ppos1.x, ppos1.y, ppos1.z,
				(hit ? "" : "")
			);
		}

		if (hit) {
			if (replayCheckpointTrace || unit->id == replayCheckpointDebugUnitID) {
				const float3 hitPos = cq.GetHitPos();
				const LocalModelPiece* hitPiece = cq.GetHitPiece();
				const char* hitPieceName = (hitPiece != nullptr && hitPiece->original != nullptr) ? hitPiece->original->name.c_str() : "<none>";
				LOG("[ReplayCheckpoint][proj-collision] hit frame=%d projectile=%d index=%u unit=%d inside=%d ingress=%d egress=%d hitPiece=%d hitPieceModel=%d hitPieceScript=%d hitPieceName=%s hitPos=<%f,%f,%f>",
					gs->frameNum,
					p->id,
					static_cast<unsigned int>(unitIndex),
					unit->id,
					static_cast<int>(cq.InsideHit()),
					static_cast<int>(cq.IngressHit()),
					static_cast<int>(cq.EgressHit()),
					static_cast<int>(hitPiece != nullptr),
					(hitPiece != nullptr) ? hitPiece->GetLModelPieceIndex() : -1,
					(hitPiece != nullptr) ? hitPiece->GetScriptPieceIndex() : -1,
					hitPieceName,
					hitPos.x, hitPos.y, hitPos.z
				);
			}

			if (cq.GetHitPiece() != nullptr)
				unit->SetLastHitPiece(cq.GetHitPiece(), gs->frameNum, p->synced);

			if (!cq.InsideHit()) {
				p->SetPosition(cq.GetHitPos());
				ReplayCheckpointLogProjectileEvent("collision-unit", p);
				p->Collision(unit);
				p->SetPosition(ppos0);
			} else {
				ReplayCheckpointLogProjectileEvent("collision-unit-inside", p);
				p->Collision(unit);
			}

			break;
		}
	}
}

void CProjectileHandler::CheckFeatureCollisions(
	CProjectile* p,
	std::vector<CFeature*>& tempFeatures,
	const float3 ppos0,
	const float3 ppos1
) {
	RECOIL_DETAILED_TRACY_ZONE;
	// already collided with unit?
	if (!p->checkCol)
		return;

	if ((p->GetCollisionFlags() & Collision::NOFEATURES) != 0)
		return;

	CollisionQuery cq;

	for (CFeature* feature: tempFeatures) {
		assert(feature != nullptr);

		if (!feature->HasCollidableStateBit(CSolidObject::CSTATE_BIT_PROJECTILES))
			continue;

		if (CCollisionHandler::DetectHit(feature, feature->GetTransformMatrix(true), ppos0, ppos1, &cq)) {
			if (cq.GetHitPiece() != nullptr)
				feature->SetLastHitPiece(cq.GetHitPiece(), gs->frameNum, p->synced);

			if (!cq.InsideHit()) {
				p->SetPosition(cq.GetHitPos());
				ReplayCheckpointLogProjectileEvent("collision-feature", p);
				p->Collision(feature);
				p->SetPosition(ppos0);
			} else {
				ReplayCheckpointLogProjectileEvent("collision-feature-inside", p);
				p->Collision(feature);
			}

			break;
		}
	}
}


void CProjectileHandler::CheckShieldCollisions(
	CProjectile* p,
	std::vector<CPlasmaRepulser*>& tempRepulsers,
	const float3 ppos0,
	const float3 ppos1
) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (!p->checkCol)
		return;
	// skip unsynced and non-weapon projectiles
	if (!p->weapon)
		return;

	CWeaponProjectile* wpro = static_cast<CWeaponProjectile*>(p);
	const WeaponDef* wdef = wpro->GetWeaponDef();

	const unsigned int interceptType = wdef->interceptedByShieldType;
	const unsigned int projAllyTeam = p->GetAllyteamID();

	// bail early
	if (interceptType == 0)
		return;

	CollisionQuery cq;

	for (CPlasmaRepulser* repulser: tempRepulsers) {
		assert(repulser != nullptr);

		if (!repulser->CanIntercept(interceptType, projAllyTeam))
			continue;

		// we sometimes get false inside hits due to the movement of the shield
		// a very hacky solution is to nudge the start of the intersecting ray
		// back (proportional to how far the shield moved last frame) so as to
		// increase its length.
		// it's not 100% accurate so there's a bit of a FIXME here to do a real
		// solution (keep track in the projectile which shields it's in)
		const float3 rpvec  = ppos0 - ppos1;
		const float3 rppos0 = ppos0 + rpvec * repulser->GetDeltaDist();
		const float3 cvpos  = repulser->weaponMuzzlePos - repulser->owner->relMidPos;

		// shield volumes are always spherical, transform directly
		// (CollisionHandler will cancel out the relmidpos offset)
		if (!CCollisionHandler::DetectHit(repulser->owner, &repulser->collisionVolume, CMatrix44f{cvpos}, rppos0, ppos1, &cq))
			continue;

		if (cq.InsideHit() && repulser->IgnoreInteriorHit(wpro))
			continue;

		if (repulser->IncomingProjectile(wpro, cq.GetHitPos()))
			return;
	}
}

void CProjectileHandler::CheckUnitFeatureCollisions(bool synced)
{
	RECOIL_DETAILED_TRACY_ZONE;
	static std::vector<CUnit*> tempUnits;
	static std::vector<CFeature*> tempFeatures;
	static std::vector<CPlasmaRepulser*> tempRepulsers;

	//can't use iterators here, because instructions inside the loop modify projectiles[synced]
	for (size_t i = 0; i < projectiles[synced].size(); ++i) {
		CProjectile* p = projectiles[synced][i];

		if (!p->checkCol) continue;
		if ( p->deleteMe) continue;

		const float3 ppos0 = p->pos;
		const float3 ppos1 = p->pos + p->speed;
		// const float3 ppos1 = p->pos + p->dir * (p->speed.w + p->radius);

		quadField.GetUnitsAndFeaturesColVol(p->pos, p->speed.w + p->radius, tempUnits, tempFeatures, &tempRepulsers);

		CheckShieldCollisions (p, tempRepulsers, ppos0, ppos1); tempRepulsers.clear();
		CheckUnitCollisions   (p, tempUnits    , ppos0, ppos1); tempUnits.clear();
		CheckFeatureCollisions(p, tempFeatures , ppos0, ppos1); tempFeatures.clear();
	}
}

void CProjectileHandler::CheckGroundCollisions(bool synced)
{
	RECOIL_DETAILED_TRACY_ZONE;
	//can't use iterators here, because instructions inside the loop modify projectiles[synced]
	for (size_t i = 0; i < projectiles[synced].size(); ++i) {
		CProjectile* p = projectiles[synced][i];

		if (!p->checkCol)
			continue;

		// NOTE:
		//   if <p> is a MissileProjectile and does not have
		//   selfExplode set, tbis will cause it to never be
		//   removed (!)
		if (p->GetCollisionFlags() & Collision::NOGROUND)
			continue;

		// don't collide with ground yet if last update scheduled a bounce
		if (p->weapon && static_cast<const CWeaponProjectile*>(p)->HasScheduledBounce())
			continue;

		// NOTE:
		//   don't add p->radius to groundHeight, or most (esp. modelled)
		//   projectiles will collide with the ground one or more frames
		//   too early
		const float& px = p->pos.x;
		const float& py = p->pos.y;
		const float& pz = p->pos.z;

		const float gy = CGround::GetHeightReal(px, pz);

		const bool belowGround = (py < gy);
		const bool insideWater = (py <= CGround::GetWaterLevel(px, pz));

		if (!belowGround && (!insideWater || p->ignoreWater))
			continue;

		if likely(belowGround) {
			//ZoneScopedN("CheckGroundCollisions::BG");
			if likely(p->speed.w > 0 && !p->blockPreciseCol) {
				const auto& prePos = p->preFrameTra.t;
				const auto groundDistance = std::clamp(CGround::LineGroundCol(prePos, p->pos, synced), 0.0f, p->speed.w);
				p->SetPosition(prePos + static_cast<float3>(p->speed) * groundDistance / p->speed.w);
			}
			else {
				p->pos.y = gy;
			}
		}

		ReplayCheckpointLogProjectileEvent("collision-ground", p);
		p->Collision();
	}
}

void CProjectileHandler::CheckCollisions()
{
	SCOPED_TIMER("Sim::Projectiles::Collisions");

	CheckUnitFeatureCollisions(true ); // changes simulation state
	CheckUnitFeatureCollisions(false); // does not change simulation state

	CheckGroundCollisions(true ); // changes simulation state
	CheckGroundCollisions(false); // does not change simulation state
}



void CProjectileHandler::AddFlyingPiece(
	int modelType,
	const S3DModelPiece* piece,
	const CMatrix44f& m,
	const float3 pos,
	const float3 speed,
	const float2 pieceParams,
	const int2 renderParams
) {
	RECOIL_DETAILED_TRACY_ZONE;
	flyingPieces[modelType].emplace_back(piece, m, pos, speed, pieceParams, renderParams);
	resortFlyingPieces[modelType] = true;
}


void CProjectileHandler::AddNanoParticle(
	const float3 startPos,
	const float3 endPos,
	const UnitDef* unitDef,
	int teamNum,
	bool highPriority
) {
	RECOIL_DETAILED_TRACY_ZONE;
	const float priority = mix(NORMAL_NANO_PRIO, HIGH_NANO_PRIO, highPriority);
	const float emitProb = 1.0f - GetNanoParticleSaturation(priority);

	if (emitProb < guRNG.NextFloat())
		return;
	if (!unitDef->showNanoSpray)
		return;

	float3 dif = endPos - startPos;
	const float l = fastmath::apxsqrt2(dif.SqLength());

	dif /= l;
	dif += (guRNG.NextVector() * 0.15f);

	const     float3 udColor = unitDef->nanoColor;
	constexpr float  udAlpha = 20 / 256.0f; // denom=255 is not constexpr-able

	const     uint8_t* tColor = (teamHandler.Team(teamNum))->color;
	constexpr uint8_t  tAlpha = udAlpha * 256;

	const SColor colors[2] = {
		{udColor.r, udColor.g, udColor.b, udAlpha},
		{tColor[0], tColor[1], tColor[2],  tAlpha},
	};

	projMemPool.alloc<CNanoProjectile>(startPos, dif, int(l), colors[globalRendering->teamNanospray]);
}

void CProjectileHandler::AddNanoParticle(
	const float3 startPos,
	const float3 endPos,
	const UnitDef* unitDef,
	int teamNum,
	float radius,
	bool inverse,
	bool highPriority
) {
	RECOIL_DETAILED_TRACY_ZONE;
	const float priority = mix(NORMAL_NANO_PRIO, HIGH_NANO_PRIO, highPriority);
	const float emitProb = 1.0f - GetNanoParticleSaturation(priority);

	if (emitProb < guRNG.NextFloat())
		return;
	if (!unitDef->showNanoSpray)
		return;

	float3 dif = endPos - startPos;
	const float len = fastmath::apxsqrt2(dif.SqLength());

	dif /= len;
	dif += (guRNG.NextVector() * (radius / len));

	const     float3 udColor = unitDef->nanoColor;
	constexpr float  udAlpha = 20 / 256.0f;

	const     uint8_t* tColor = (teamHandler.Team(teamNum))->color;
	constexpr uint8_t  tAlpha = udAlpha * 256;

	const SColor colors[2] = {
		{udColor.r, udColor.g, udColor.b, udAlpha},
		{tColor[0], tColor[1], tColor[2],  tAlpha},
	};

	if (!inverse) {
		projMemPool.alloc<CNanoProjectile>(startPos, dif * 3.0f, int(len / 3.0f), colors[globalRendering->teamNanospray]);
	} else {
		projMemPool.alloc<CNanoProjectile>(startPos + dif * len, -dif * 3.0f, int(len / 3.0f), colors[globalRendering->teamNanospray]);
	}
}

float CProjectileHandler::GetParticleSaturation(bool randomized) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const int curParticles = GetCurrentParticles();

	// use the random mult to weaken the max limit a little
	// so the chance is better spread when being close to the limit
	// i.e. when there are rockets that spam CEGs this gives smaller CEGs still a chance
	const float total = std::max(1.0f, maxParticles * 1.0f);
	const float fract = curParticles / total;
	const float rmult = 1.0f + (int(randomized) * 0.3f * guRNG.NextFloat());

	return (fract * rmult);
}

int CProjectileHandler::GetCurrentParticles() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// use precached part of particles count calculation that else becomes very heavy
	// example where it matters: (in ZK) /cheat /give 20 armraven -> shoot ground
	for (size_t i = frameProjectileCounts[true], e = projectiles[true].size(); i < e; ++i) {
		frameCurrentParticles += projectiles[true][i]->GetProjectilesCount();
	}
	frameProjectileCounts[true ] = projectiles[true ].size();

	for (size_t i = frameProjectileCounts[false], e = projectiles[false].size(); i < e; ++i) {
		frameCurrentParticles += projectiles[false][i]->GetProjectilesCount();
	}
	frameProjectileCounts[false] = projectiles[false].size();

	int partCount = frameCurrentParticles;
	for (const auto& c: flyingPieces) {
		for (const auto& fp: c) {
			partCount += fp.GetDrawCallCount();
		}
	}
	partCount += groundFlashes.size();
	return partCount;
}
