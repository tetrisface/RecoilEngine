/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "BeamLaser.h"
#include "PlasmaRepulser.h"
#include "WeaponDef.h"
#include "Game/GameHelper.h"
#include "Game/TraceRay.h"
#include "Map/Ground.h"
#include "Rendering/Models/3DModelPiece.hpp"
#include "Rendering/Models/LocalModelPiece.hpp"
#include "Sim/Features/Feature.h"
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/StrafeAirMoveType.h"
#include "Sim/Projectiles/WeaponProjectiles/WeaponProjectileFactory.h"
#include "Sim/Units/Scripts/UnitScript.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/SpringMath.h"

#include "System/Misc/TracyDefs.h"

#include <cstdint>
#include <cstring>
#include <vector>

#define SWEEPFIRE_ENABLED 1

static bool ReplayCheckpointDebugBeamLaserFrame()
{
	const int debugFrame = configHandler->GetInt("ReplayCheckpointDebugSignatureFrame");
	return (debugFrame >= 0 && gs != nullptr && gs->frameNum == debugFrame);
}

static uint32_t ReplayCheckpointBeamFloatBits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static const char* ReplayCheckpointBeamPieceName(const LocalModelPiece* piece)
{
	if (piece == nullptr || piece->original == nullptr)
		return "";

	return piece->original->name.c_str();
}

CR_BIND_DERIVED(CBeamLaser, CWeapon, )

CR_REG_METADATA(CBeamLaser,(
	CR_MEMBER(color),
	CR_MEMBER(oldDir),

	CR_MEMBER(salvoDamageMult),
	CR_MEMBER(sweepFireState)
))

CR_BIND(CBeamLaser::SweepFireState, )
CR_REG_METADATA_SUB(CBeamLaser, SweepFireState, (
	CR_MEMBER(sweepInitPos),
	CR_MEMBER(sweepGoalPos),
	CR_MEMBER(sweepInitDir),
	CR_MEMBER(sweepGoalDir),
	CR_MEMBER(sweepCurrDir),
	CR_MEMBER(sweepTempDir),
	CR_MEMBER(sweepInitDst),
	CR_MEMBER(sweepGoalDst),
	CR_MEMBER(sweepCurrDst),
	CR_MEMBER(sweepStartAngle),
	CR_MEMBER(sweepFiring)
))



void CBeamLaser::SweepFireState::Init(const float3& newTargetPos, const float3& muzzlePos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	sweepInitPos = sweepGoalPos;
	sweepInitDst = (sweepInitPos - muzzlePos).Length2D();

	sweepGoalPos = newTargetPos;
	sweepGoalDst = (sweepGoalPos - muzzlePos).Length2D();
	sweepCurrDst = sweepInitDst;

	sweepInitDir = (sweepInitPos - muzzlePos).SafeNormalize();
	sweepGoalDir = (sweepGoalPos - muzzlePos).SafeNormalize();

	sweepStartAngle = math::acosf(std::clamp(sweepInitDir.dot(sweepGoalDir), -1.0f, 1.0f));
	sweepFiring = true;
}

float CBeamLaser::SweepFireState::GetTargetDist2D() const {
	RECOIL_DETAILED_TRACY_ZONE;
	if (sweepStartAngle < 0.01f)
		return sweepGoalDst;

	const float sweepCurAngleCos = sweepCurrDir.dot(sweepGoalDir);
	const float sweepCurAngleRad = math::acosf(std::clamp(sweepCurAngleCos, -1.0f, 1.0f));

	// goes from 1 to 0 as the angular difference decreases during the sweep
	const float sweepAngleAlpha = (std::clamp(sweepCurAngleRad / sweepStartAngle, 0.0f, 1.0f));

	// get the linearly-interpolated beam length for this point of the sweep
	return (mix(sweepInitDst, sweepGoalDst, 1.0f - sweepAngleAlpha));
}



CBeamLaser::CBeamLaser(CUnit* owner, const WeaponDef* def)
	: CWeapon(owner, def)
	, salvoDamageMult(1.0f)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// null happens when loading
	if (def != nullptr)
		color = def->visuals.color;

	sweepFireState.SetDamageAllies((collisionFlags & Collision::NOFRIENDLIES) == 0);
}



void CBeamLaser::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!weaponDef->beamburst) {
		salvoDelay = 0;
		salvoSize = int(weaponDef->beamtime * GAME_SPEED);
		salvoSize = std::max(salvoSize, 1);

		// multiply damage with this on each shot so the total damage done is correct
		salvoDamageMult = 1.0f / salvoSize;
	}

	CWeapon::Init();

	muzzleFlareSize = 0.0f;
}

void CBeamLaser::UpdatePosAndMuzzlePos()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (sweepFireState.IsSweepFiring()) {
		const int weaponPiece = owner->script->QueryWeapon(weaponNum);
		const auto weaponMat = owner->script->GetPieceMatrix(weaponPiece);

		const float3 relWeaponPos = weaponMat.GetPos();
		const float3 newWeaponDir = owner->GetObjectSpaceVec(float3(weaponMat[2], weaponMat[6], weaponMat[10]));

		relWeaponMuzzlePos = owner->script->GetPiecePos(weaponPiece);

		aimFromPos = owner->GetObjectSpacePos(relWeaponPos * float3(-1.0f, 1.0f, -1.0f)); // ??
		weaponMuzzlePos = owner->GetObjectSpacePos(relWeaponMuzzlePos);

		sweepFireState.SetSweepTempDir(newWeaponDir);
	} else {
		UpdateWeaponVectors();

		if (weaponDef->sweepFire) {
			// needed for first call to GetFireDir() when new sweep starts after inactivity
			sweepFireState.SetSweepTempDir((weaponMuzzlePos - aimFromPos).SafeNormalize());
		}
	}
}

float CBeamLaser::GetPredictedImpactTime(const float3& p) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// beamburst tracks the target during the burst so there's no need to lead
	return (salvoSize * 0.5f * (1 - weaponDef->beamburst));
}

void CBeamLaser::UpdateSweep()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// sweeping always happens between targets
	if (!HaveTarget()) {
		sweepFireState.SetSweepFiring(false);
		return;
	}
	if (!weaponDef->sweepFire)
		return;

	#if (!SWEEPFIRE_ENABLED)
	return;
	#endif

	// if current target position changed, start sweeping through a new arc
	if (sweepFireState.StartSweep(currentTargetPos))
		sweepFireState.Init(currentTargetPos, weaponMuzzlePos);

	if (sweepFireState.IsSweepFiring())
		sweepFireState.Update(GetFireDir(true, false));

	// TODO:
	//   also stop sweep if angle no longer changes, spawn
	//   more intermediate beams for large angle and range?
	if (sweepFireState.StopSweep())
		sweepFireState.SetSweepFiring(false);

	if (!sweepFireState.IsSweepFiring())
		return;
	if (reloadStatus > gs->frameNum)
		return;

	/* FIXME: checking for the full amount but only consuming
	 * a fraction looks odd, could use a good looking at. */
	const auto team = teamHandler.Team(owner->team);
	if (!team->HaveResources(weaponDef->cost))
		return;
	if (!team->UseResources(weaponDef->cost / salvoSize))
		return;

	FireInternal(sweepFireState.GetSweepCurrDir());

	// FIXME:
	//   reloadStatus is normally only set in UpdateFire and only if CanFire
	//   (which is not true during sweeping, the integration should be better)
	reloadStatus = gs->frameNum + int(reloadTime / owner->reloadSpeed);
}

void CBeamLaser::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;
	UpdatePosAndMuzzlePos();
	CWeapon::Update();
	UpdateSweep();
}

float3 CBeamLaser::GetFireDir(bool sweepFire, bool scriptCall)
{
	RECOIL_DETAILED_TRACY_ZONE;
	float3 dir = currentTargetPos - weaponMuzzlePos;

	if (!sweepFire) {
		if (scriptCall) {
			dir = dir.SafeNormalize();
		} else {
			if (onlyForward && owner->unitDef->IsStrafingAirUnit()) {
				// [?] StrafeAirMovetype cannot align itself properly, change back when that is fixed
				dir = owner->frontdir;
			} else {
				if (salvoLeft == salvoSize - 1) {
					oldDir = (dir = dir.SafeNormalize());
				} else if (weaponDef->beamburst) {
					dir = dir.SafeNormalize();
				} else {
					dir = oldDir;
				}
			}

			dir += SalvoErrorExperience();
			dir.SafeNormalize();

			// NOTE:
			//  on units with (extremely) long weapon barrels the muzzle
			//  can be on the far side of the target unit such that <dir>
			//  would point away from it
			if ((currentTargetPos - weaponMuzzlePos).dot(currentTargetPos - owner->aimPos) < 0.0f) {
				dir = -dir;
			}
		}
	} else {
		// need to emit the sweeping beams from the right place
		// NOTE:
		//   this way of implementing sweep-fire is extremely bugged
		//   the intersection points traced by rays from the turning
		//   weapon piece do NOT describe a fluid arc segment between
		//   old and new target positions (nor even a straight line)
		//   --> animation scripts cannot be relied upon to smoothly
		//   vary pitch of the weapon muzzle piece so use workaround
		//
		dir = sweepFireState.GetSweepTempDir();
		dir.Normalize2D();

		const float3 tgtPos = float3(dir.x * sweepFireState.GetTargetDist2D(), 0.0f, dir.z * sweepFireState.GetTargetDist2D());
		const float tgtHgt = CGround::GetHeightReal(weaponMuzzlePos.x + tgtPos.x, weaponMuzzlePos.z + tgtPos.z);

		// NOTE: INTENTIONALLY NOT NORMALIZED HERE
		dir = (tgtPos + UpVector * (tgtHgt - weaponMuzzlePos.y));
	}

	return dir;
}

void CBeamLaser::FireImpl(const bool scriptCall)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// sweepfire must exclude regular fire (!)
	if (sweepFireState.IsSweepFiring())
		return;

	FireInternal(GetFireDir(false, scriptCall));
}

bool CBeamLaser::TestRange(const float3& tgtPos, const SWeaponTarget& trg) const
{
	float3 aimDir = (tgtPos - aimFromPos);
	const float targetDist = aimDir.LengthNormalize();

	if (const auto shapedRange = GetShapedWeaponRange(aimDir, range); targetDist > shapedRange)
		return false;

	// NOTE: mainDir is in unit-space
	return (CheckTargetAngleConstraint(aimDir, owner->GetObjectSpaceVec(mainDir)));
}

void CBeamLaser::FireInternal(float3 curDir)
{
	RECOIL_DETAILED_TRACY_ZONE;
	float actualRange = range;

	/* FPS mode targeting essentially behaves as if with targetBorder=1,
	 * even for units without this property. The practical effect is that
	 * you can FPS a beamlaser turret and shoot units (esp. enemy turrets
 	 * of the same type) that normal target acquisition would refuse to.
	 *
	 * This arbitrary -5% range penalty aims to mitigate this. It relies
	 * on the relative sizes of hitvolumes being circa about that big in
	 * relation to unit ranges, which happens to be valid in games that
	 * keep FPS mode available.
  	 *
 	 * Long-term it would be good to do something definitive about FPS
    	 * mode (embrace or deprecate) but for now it doesn't hurt too much
       	 * to keep this. */
	float rangeMod = 1.0f - (0.05f * owner->UnderFirstPersonControl());

	bool tryAgain = true;
	bool doDamage = true;

	float maxLength = range * rangeMod;
	float curLength = 0.0f;

	float3 curPos = weaponMuzzlePos;
	float3 hitPos;
	float3 newDir;

	// objects at the end of the beam
	CUnit* hitUnit = nullptr;
	CFeature* hitFeature = nullptr;
	CPlasmaRepulser* hitShield = nullptr;
	static std::vector<TraceRay::SShieldDist> hitShields;
	CollisionQuery hitColQuery;

	const bool debugBeamLaserFrame = ReplayCheckpointDebugBeamLaserFrame();

	if (!sweepFireState.IsSweepFiring()) {
		const uint64_t rngStateBefore = gsRNG.GetGenState();
		const uint64_t rngSeqBefore = gsRNG.GetGenSequence();
		const float3 sprayVector = gsRNG.NextVector();
		const float sprayAngle = SprayAngleExperience();
		curDir += (sprayVector * sprayAngle);
		curDir.SafeNormalize();

		maxLength = GetShapedWeaponRange(curDir, maxLength);

		if (debugBeamLaserFrame) {
			LOG("[ReplayCheckpoint][beamlaser] spray frame=%d owner=%d weapon=%d def=%u rngBefore=%llu/%llu rngAfter=%llu/%llu sprayAngle=%.9g sprayAngleBits=%08x spray=<%.9g,%.9g,%.9g> sprayBits=<%08x,%08x,%08x> curDir=<%.9g,%.9g,%.9g> curDirBits=<%08x,%08x,%08x> maxLength=%.9g maxLengthBits=%08x muzzle=<%.9g,%.9g,%.9g> target=<%.9g,%.9g,%.9g>",
				gs->frameNum,
				owner->id,
				weaponNum,
				weaponDef != nullptr ? weaponDef->id : 0u,
				static_cast<unsigned long long>(rngStateBefore),
				static_cast<unsigned long long>(rngSeqBefore),
				static_cast<unsigned long long>(gsRNG.GetGenState()),
				static_cast<unsigned long long>(gsRNG.GetGenSequence()),
				sprayAngle,
				ReplayCheckpointBeamFloatBits(sprayAngle),
				sprayVector.x, sprayVector.y, sprayVector.z,
				ReplayCheckpointBeamFloatBits(sprayVector.x),
				ReplayCheckpointBeamFloatBits(sprayVector.y),
				ReplayCheckpointBeamFloatBits(sprayVector.z),
				curDir.x, curDir.y, curDir.z,
				ReplayCheckpointBeamFloatBits(curDir.x),
				ReplayCheckpointBeamFloatBits(curDir.y),
				ReplayCheckpointBeamFloatBits(curDir.z),
				maxLength,
				ReplayCheckpointBeamFloatBits(maxLength),
				weaponMuzzlePos.x, weaponMuzzlePos.y, weaponMuzzlePos.z,
				currentTargetPos.x, currentTargetPos.y, currentTargetPos.z);
		}
	}
	else {
		// restrict the range when sweeping
		maxLength = std::min(maxLength, sweepFireState.GetTargetDist3D() * 1.125f);

		if (debugBeamLaserFrame) {
			LOG("[ReplayCheckpoint][beamlaser] sweep frame=%d owner=%d weapon=%d def=%u curDir=<%.9g,%.9g,%.9g> curDirBits=<%08x,%08x,%08x> maxLength=%.9g maxLengthBits=%08x muzzle=<%.9g,%.9g,%.9g> target=<%.9g,%.9g,%.9g>",
				gs->frameNum,
				owner->id,
				weaponNum,
				weaponDef != nullptr ? weaponDef->id : 0u,
				curDir.x, curDir.y, curDir.z,
				ReplayCheckpointBeamFloatBits(curDir.x),
				ReplayCheckpointBeamFloatBits(curDir.y),
				ReplayCheckpointBeamFloatBits(curDir.z),
				maxLength,
				ReplayCheckpointBeamFloatBits(maxLength),
				weaponMuzzlePos.x, weaponMuzzlePos.y, weaponMuzzlePos.z,
				currentTargetPos.x, currentTargetPos.y, currentTargetPos.z);
		}
	}


	uint32_t lastProjID = -1;
	for (int tries = 0; tries < 5 && tryAgain; ++tries) {
		float beamLength = TraceRay::TraceRay(curPos, curDir, maxLength - curLength, collisionFlags, owner, hitUnit, hitFeature, &hitColQuery);
		const float rawBeamLength = beamLength;
		const CUnit* rawHitUnit = hitUnit;
		const CFeature* rawHitFeature = hitFeature;
		const LocalModelPiece* rawHitPiece = hitColQuery.GetHitPiece();
		const float3 rawHitPos = hitColQuery.GetHitPos();

		if (debugBeamLaserFrame) {
			const Transform* pieceTransform = (rawHitPiece != nullptr) ? &rawHitPiece->GetModelSpaceTransformRaw() : nullptr;
			const float3 piecePos = (rawHitPiece != nullptr) ? rawHitPiece->GetPosition() : ZeroVector;
			const float3 pieceRot = (rawHitPiece != nullptr) ? rawHitPiece->GetRotation() : ZeroVector;
			const float pieceScale = (rawHitPiece != nullptr) ? rawHitPiece->GetScaling() : 0.0f;
			const float3 pieceTraPos = (pieceTransform != nullptr) ? pieceTransform->t : ZeroVector;
			const CQuaternion pieceTraRot = (pieceTransform != nullptr) ? pieceTransform->r : CQuaternion();
			const float pieceTraScale = (pieceTransform != nullptr) ? pieceTransform->s : 0.0f;

			LOG("[ReplayCheckpoint][beamlaser] trace frame=%d owner=%d weapon=%d def=%u try=%d curPos=<%.9g,%.9g,%.9g> curPosBits=<%08x,%08x,%08x> curDir=<%.9g,%.9g,%.9g> curDirBits=<%08x,%08x,%08x> traceLength=%.9g traceLengthBits=%08x beamLength=%.9g beamLengthBits=%08x rawHitUnit=%d rawHitFeature=%d rawHitPieceModel=%d rawHitPieceScript=%d rawHitPieceName=%s ingress=%u egress=%u inside=%u rawHitPos=<%.9g,%.9g,%.9g> rawHitPosBits=<%08x,%08x,%08x> piecePos=<%.9g,%.9g,%.9g> pieceRot=<%.9g,%.9g,%.9g> pieceScale=%.9g pieceTraPos=<%.9g,%.9g,%.9g> pieceTraRot=<%.9g,%.9g,%.9g,%.9g> pieceTraScale=%.9g",
				gs->frameNum,
				owner->id,
				weaponNum,
				weaponDef != nullptr ? weaponDef->id : 0u,
				tries,
				curPos.x, curPos.y, curPos.z,
				ReplayCheckpointBeamFloatBits(curPos.x),
				ReplayCheckpointBeamFloatBits(curPos.y),
				ReplayCheckpointBeamFloatBits(curPos.z),
				curDir.x, curDir.y, curDir.z,
				ReplayCheckpointBeamFloatBits(curDir.x),
				ReplayCheckpointBeamFloatBits(curDir.y),
				ReplayCheckpointBeamFloatBits(curDir.z),
				maxLength - curLength,
				ReplayCheckpointBeamFloatBits(maxLength - curLength),
				rawBeamLength,
				ReplayCheckpointBeamFloatBits(rawBeamLength),
				rawHitUnit != nullptr ? rawHitUnit->id : -1,
				rawHitFeature != nullptr ? rawHitFeature->id : -1,
				rawHitPiece != nullptr ? rawHitPiece->GetLModelPieceIndex() : -1,
				rawHitPiece != nullptr ? rawHitPiece->GetScriptPieceIndex() : -1,
				ReplayCheckpointBeamPieceName(rawHitPiece),
				hitColQuery.IngressHit() ? 1u : 0u,
				hitColQuery.EgressHit() ? 1u : 0u,
				hitColQuery.InsideHit() ? 1u : 0u,
				rawHitPos.x, rawHitPos.y, rawHitPos.z,
				ReplayCheckpointBeamFloatBits(rawHitPos.x),
				ReplayCheckpointBeamFloatBits(rawHitPos.y),
				ReplayCheckpointBeamFloatBits(rawHitPos.z),
				piecePos.x, piecePos.y, piecePos.z,
				pieceRot.x, pieceRot.y, pieceRot.z,
				pieceScale,
				pieceTraPos.x, pieceTraPos.y, pieceTraPos.z,
				pieceTraRot.x, pieceTraRot.y, pieceTraRot.z, pieceTraRot.r,
				pieceTraScale);
		}

		if (hitUnit != nullptr && teamHandler.AlliedTeams(hitUnit->team, owner->team)) {
			if (sweepFireState.IsSweepFiring() && !sweepFireState.DamageAllies()) {
				doDamage = false; break;
			}
		}

		if (!weaponDef->waterweapon) {
			// terminate beam at water surface if necessary
			if ((curDir.y < 0.0f) && ((curPos.y + curDir.y * beamLength) <= 0.0f)) {
				beamLength = curPos.y / -curDir.y;

				hitUnit = nullptr;
				hitFeature = nullptr;
			}
		}

		// if the beam gets intercepted, this modifies newDir
		//
		// we do more than one trace-iteration and set dir to
		// newDir only in the case there is a shield in our way
		hitShields.clear();
		TraceRay::TraceRayShields(this, curPos, curDir, beamLength, hitShields);

		for (const TraceRay::SShieldDist& sd: hitShields) {
			if (sd.dist < beamLength && sd.rep->IncomingBeam(this, curPos, curPos + (curDir * sd.dist), salvoDamageMult)) {
				beamLength = sd.dist;

				hitUnit = nullptr;
				hitFeature = nullptr;
				hitShield = sd.rep;
				break;
			}
		}

		// same as hitColQuery.GetHitPos() if no water or shield in way
		hitPos = curPos + curDir * beamLength;

		if (debugBeamLaserFrame) {
			LOG("[ReplayCheckpoint][beamlaser] segment frame=%d owner=%d weapon=%d def=%u try=%d rawBeamLength=%.9g rawBeamLengthBits=%08x finalBeamLength=%.9g finalBeamLengthBits=%08x hitUnit=%d hitFeature=%d hitShield=%d shieldHits=%u hitPos=<%.9g,%.9g,%.9g> hitPosBits=<%08x,%08x,%08x> curLength=%.9g maxLength=%.9g",
				gs->frameNum,
				owner->id,
				weaponNum,
				weaponDef != nullptr ? weaponDef->id : 0u,
				tries,
				rawBeamLength,
				ReplayCheckpointBeamFloatBits(rawBeamLength),
				beamLength,
				ReplayCheckpointBeamFloatBits(beamLength),
				hitUnit != nullptr ? hitUnit->id : -1,
				hitFeature != nullptr ? hitFeature->id : -1,
				hitShield != nullptr ? 1 : 0,
				static_cast<unsigned int>(hitShields.size()),
				hitPos.x, hitPos.y, hitPos.z,
				ReplayCheckpointBeamFloatBits(hitPos.x),
				ReplayCheckpointBeamFloatBits(hitPos.y),
				ReplayCheckpointBeamFloatBits(hitPos.z),
				curLength,
				maxLength);
		}

		if (hitShield != nullptr && hitShield->weaponDef->shieldRepulser) {
			// reflect
			const float3 normal = (hitPos - hitShield->weaponMuzzlePos).Normalize();
			const float3 prjDir = normal * normal.dot(curDir) * 2.0f;

			newDir = curDir - prjDir;
			tryAgain = true;
			hitShield = nullptr;
		} else {
			tryAgain = false;
		}

		{
			const float baseAlpha  = weaponDef->intensity * 255.0f;
			const float startAlpha = (1.0f - (curLength             ) / maxLength);
			const float endAlpha   = (1.0f - (curLength + beamLength) / maxLength);

			ProjectileParams pparams = GetProjectileParams();
			pparams.pos = curPos;
			pparams.end = hitPos;
			pparams.ttl = weaponDef->beamLaserTTL;
			pparams.startAlpha = std::clamp(startAlpha * baseAlpha, 0.0f, 255.0f);
			pparams.endAlpha = std::clamp(endAlpha * baseAlpha, 0.0f, 255.0f);

			lastProjID = WeaponProjectileFactory::LoadProjectile(pparams);
		}

		curPos = hitPos;
		curDir = newDir;
		curLength += beamLength;
	}

	if (!doDamage)
		return;

	if (hitUnit != nullptr) {
		hitUnit->SetLastHitPiece(hitColQuery.GetHitPiece(), gs->frameNum);

		// FIXME? still assumes spherical CV's
		actualRange += (hitUnit->radius * weaponDef->targetBorder * (weaponDef->targetBorder > 0.0f));
	}

	if (curLength < maxLength) {
		// make it possible to always hit with some minimal intensity (melee weapons have use for that)
		const float hitIntensity = std::max(weaponDef->minIntensity, 1.0f - curLength / (actualRange * 2.0f));

		const DamageArray& baseDamages = damages->GetDynamicDamages(weaponMuzzlePos, curPos);
		const DamageArray da = baseDamages * (hitIntensity * salvoDamageMult);
		const CExplosionParams params = {
			.pos                  = hitPos,
			.dir                  = curDir,
			.damages              = da,
			.weaponDef            = weaponDef,
			.owner                = owner,
			.hitObject            = ExplosionHitObject(hitUnit, hitFeature, hitShield),
			.craterAreaOfEffect   = damages->craterAreaOfEffect,
			.damageAreaOfEffect   = damages->damageAreaOfEffect,
			.edgeEffectiveness    = damages->edgeEffectiveness,
			.explosionSpeed       = damages->explosionSpeed,
			.gfxMod               = 1.0f,
			.maxGroundDeformation = 0.0f,
			.impactOnly           = weaponDef->impactOnly,
			.ignoreOwner          = weaponDef->noExplode || weaponDef->noSelfDamage,
			.damageGround         = true,
			.projectileID         = lastProjID
		};

		helper->Explosion(params);
	}
}
