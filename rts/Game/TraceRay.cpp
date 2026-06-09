/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "TraceRay.h"
#include "Camera.h"
#include "GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Sim/Features/Feature.h"
#include "Sim/Misc/CollisionHandler.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Misc/GeometricObjects.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/Config/ConfigHandler.h"
#include "System/GlobalConfig.h"
#include "System/Log/ILog.h"
#include "System/SpringMath.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "System/Misc/TracyDefs.h"

//////////////////////////////////////////////////////////////////////
// Local/Helper functions
//////////////////////////////////////////////////////////////////////

static bool ReplayCheckpointDebugTraceRayFrame()
{
	const int debugFrame = configHandler->GetInt("ReplayCheckpointDebugSignatureFrame");
	return (debugFrame >= 0 && gs != nullptr && gs->frameNum == debugFrame);
}

static uint32_t ReplayCheckpointTraceRayFloatBits(float value)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

/**
 * helper for TestCone
 * @return true if object <o> is in the firing cone, false otherwise
 */
inline static bool TestConeHelper(
	const float3& tstPos,
	const float3& tstDir,
	const float length,
	const float spread,
	const CSolidObject* obj
) {
	const CollisionVolume* cv = &obj->collisionVolume;

	const float3 cvRelVec = cv->GetWorldSpacePos(obj) - tstPos;

	const float  cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float  coneSize = cvRelDst * spread + 1.0f;

	// theoretical impact position assuming no spread
	const float3 hitVec = tstDir * cvRelDst;
	const float3 hitPos = tstPos + hitVec;

	bool ret = false;

	if (obj->GetBlockingMapID() < unitHandler.MaxUnits()) {
		// obj is a unit
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CUnit*>(obj), nullptr, tstPos) - coneSize) <= 0.0f);
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CUnit*>(obj), nullptr, hitPos) - coneSize) <= 0.0f);
	} else {
		// obj is a feature
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CFeature*>(obj), nullptr, tstPos) - coneSize) <= 0.0f);
		ret = ret || ((cv->GetPointSurfaceDistance(static_cast<const CFeature*>(obj), nullptr, hitPos) - coneSize) <= 0.0f);
	}

	if (globalRendering->drawDebugTraceRay) {
		#define go geometricObjects

		if (ret) {
			go->SetColor(go->AddLine(hitPos - (UpVector * hitPos.dot(UpVector)), hitPos, 3, 1, GAME_SPEED), 1.0f, 0.0f, 0.0f, 1.0f);
		} else {
			go->SetColor(go->AddLine(hitPos - (UpVector * hitPos.dot(UpVector)), hitPos, 3, 1, GAME_SPEED), 0.0f, 1.0f, 0.0f, 1.0f);
		}

		#undef go
	}

	return ret;
}

/**
 * helper for TestTrajectoryCone
 * @return true if object <o> is in the firing trajectory, false otherwise
 */
inline static bool TestTrajectoryConeHelper(
	const float3& tstPos,
	const float3& tstDir, // 2D
	float length,
	float linear,
	float quadratic,
	float spread,
	float baseSize,
	const CSolidObject* obj
) {
	// trajectory is a parabola f(x)=a*x*x + b*x with
	// parameters a = quadratic, b = linear, and c = 0
	// (x = objDst1D, negative values represent objects
	// "behind" the testee whose collision volumes might
	// still be intersected by its trajectory arc)
	//
	// firing-cone is centered along tstDir with radius
	// <x * spread + baseSize> (usually baseSize != 0
	// so weapons with spread = 0 will test against a
	// cylinder, not an infinitely thin line as safety
	// measure against friendly-fire damage in tightly
	// packed unit groups)
	// 
	// baseSize is usually hardcoded to 0 in the functions calling this 
	//
	// return true iff the world-space point <x, f(x)>
	// lies on or inside the object's collision volume
	// (where 'x' is actually the projected xz-distance
	// to the object's colvol-center along tstDir)
	//
	// !NOTE!:
	//   THE TRAJECTORY CURVE MIGHT STILL INTERSECT
	//   EVEN WHEN <x, f(x)> DOES NOT LIE INSIDE CV
	//   SO THIS CAN GENERATE FALSE NEGATIVES
	//   Chord checks should solve this problem. 
	const CollisionVolume* cv = &obj->collisionVolume;

	const float3 cvRelVec = cv->GetWorldSpacePos(obj) - tstPos;

	const float  cvRelDst = std::clamp(cvRelVec.dot(tstDir), 0.0f, length);
	const float  coneSize = cvRelDst * spread + baseSize;

	// theoretical impact position assuming no spread
	// note that unlike TestConeHelper these positions
	// lie along curve f(x) here, not a straight line
	// (if object-distance is 0, tstPos == hitPos)
	const float3 hitVec = tstDir * cvRelDst;
	const float3 hitPos = (tstPos + hitVec) + (UpVector * (quadratic * cvRelDst * cvRelDst + linear * cvRelDst));
	float3 endPos = 0.f; 
	bool ret = false;

	CollisionQuery cq;
	// chord check to hitPos
	const CMatrix44f objTransform = obj->GetTransformMatrix(true);

	// use heuristic to choose which chord to check
	// if projectile is traveling upwards, check from muzzle to position
	// if projectile is traveling downwards, check from position to target
	if ((2 * quadratic * cvRelDst + linear) > 0) {
		if (CCollisionHandler::DetectHit(obj, objTransform, tstPos, hitPos, &cq, true)) {
			ret = true;
		}
	}
	else {
		//only compute endPos if needed
		endPos = (tstPos + tstDir * length) + (UpVector * (quadratic * length * length + linear * length));
		if (CCollisionHandler::DetectHit(obj, objTransform, hitPos, endPos, &cq, true)) {
			ret = true;
		}
	}

	if (globalRendering->drawDebugTraceRay) {

		#define go geometricObjects

		if (ret) {
			// red line pointing to hitPos
			go->SetColor(go->AddLine(tstPos + hitVec, hitPos, 3, 0, GAME_SPEED), 1.0f, 0.0f, 0.0f, 1.0f);
			// blue lines showing chord checks
			if ((2 * quadratic * cvRelDst + linear) > 0) {
				go->SetColor(go->AddLine(tstPos, hitPos, 3, 0, GAME_SPEED), 0.0f, 0.0f, 1.0f, 1.0f);
			}
			else {
				go->SetColor(go->AddLine(hitPos, endPos, 3, 0, GAME_SPEED), 0.0f, 0.0f, 1.0f, 1.0f);
			}
			// While using this debug, on SlowUpdate Frames, CWeapon::TryTargetHeading will assume the unit chassis rotates to face the target, 
			// so the tstPos will not be lined up with any part of the unit. 
			// resulting in two visible debug lines. One from the weaponmuzzle, one from the rotated weaponmuzzle
		} else {
			// green line pointing to hitPos
			go->SetColor(go->AddLine(tstPos + hitVec, hitPos, 3, 0, GAME_SPEED), 0.0f, 1.0f, 0.0f, 1.0f);
		}

		#undef go
	}

	return ret;
}



//////////////////////////////////////////////////////////////////////
// Raytracing
//////////////////////////////////////////////////////////////////////

namespace TraceRay {

// called by {CRifle, CBeamLaser, CLightningCannon}::Fire(), CWeapon::HaveFreeLineOfFire(), and Skirmish AIs
float TraceRay(const float3& p, const float3& d, float l, int f, const CUnit* o, CUnit*& hu, CFeature*& hf, CollisionQuery* cq)
{
	assert(o != nullptr);
	return (TraceRay(p, d, l, f, o->allyteam, o, hu, hf, cq));
}

float TraceRay(
	const float3& pos,
	const float3& dir,
	float traceLength,
	int traceFlags,
	int allyTeam,
	const CUnit* owner,
	CUnit*& hitUnit,
	CFeature*& hitFeature,
	CollisionQuery* hitColQuery
) {
	RECOIL_DETAILED_TRACY_ZONE;
	// NOTE:
	//   the bits here and in Test*Cone are interpreted as "do not scan for {enemy,friendly,...}
	//   objects in quads" rather than "return false if ray hits an {enemy,friendly,...} object"
	//   consequently a weapon with (e.g.) avoidFriendly=true that wants to check whether it has
	//   a free line of fire should *not* set the NOFRIENDLIES bit in its trace-flags, etc
	const bool scanForEnemies  = ((traceFlags & Collision::NOENEMIES   ) == 0);
	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForGround   = ((traceFlags & Collision::NOGROUND    ) == 0);
	const bool scanForCloaked  = ((traceFlags & Collision::NOCLOAKED   ) == 0);

	const bool scanForAnyUnits = scanForEnemies || scanForAllies || scanForNeutrals || scanForCloaked;
	const bool debugTraceRayFrame = ReplayCheckpointDebugTraceRayFrame();

	hitFeature = nullptr;
	hitUnit = nullptr;

	if (dir == ZeroVector)
		return -1.0f;

	if (scanForFeatures || scanForAnyUnits) {
		CollisionQuery cq;
		const float3 traceEnd = pos + dir * traceLength;

		QuadFieldQuery qfQuery;
		quadField.GetQuadsOnRay(qfQuery, pos, dir, traceLength);

		// locally point somewhere non-NULL; we cannot pass hitColQuery
		// to DetectHit directly because each call resets it internally
		if (hitColQuery == nullptr)
			hitColQuery = &cq;

		// feature intersection
		if (scanForFeatures) {
			for (const int quadIdx: *qfQuery.quads) {
				const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

				for (CFeature* f: quad.features) {
					// NOTE:
					//   if f is non-blocking, ProjectileHandler will not test
					//   for collisions with projectiles so we can skip it here
					if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
						continue;

					const float traceLengthBefore = traceLength;
					if (CCollisionHandler::DetectHit(f, f->GetTransformMatrix(true), pos, traceEnd, &cq, true)) {
						const float len = cq.GetHitPosDist(pos, dir);
						const bool accepted = (len < traceLength);

						if (debugTraceRayFrame) {
							const CollisionVolume& cv = f->collisionVolume;
							const float3 hitPos = cq.GetHitPos();
							const float3 featureRelMid = float3(f->relMidPos.x, f->relMidPos.y, f->relMidPos.z);

							LOG("[ReplayCheckpoint][traceray] feature-hit frame=%d owner=%d quad=%d candidate=%d accepted=%u prevLength=%.9g prevLengthBits=%08x len=%.9g lenBits=%08x pos=<%.9g,%.9g,%.9g> dir=<%.9g,%.9g,%.9g> hitPos=<%.9g,%.9g,%.9g> hitPosBits=<%08x,%08x,%08x> featurePos=<%.9g,%.9g,%.9g> featureRelMid=<%.9g,%.9g,%.9g> cvType=%d cvAxis=%d cvScales=<%.9g,%.9g,%.9g> cvOffsets=<%.9g,%.9g,%.9g> cvIgnore=%u cvCont=%u cvFoot=%u cvPiece=%u",
								gs->frameNum,
								owner != nullptr ? owner->id : -1,
								quadIdx,
								f->id,
								accepted ? 1u : 0u,
								traceLengthBefore,
								ReplayCheckpointTraceRayFloatBits(traceLengthBefore),
								len,
								ReplayCheckpointTraceRayFloatBits(len),
								pos.x, pos.y, pos.z,
								dir.x, dir.y, dir.z,
								hitPos.x, hitPos.y, hitPos.z,
								ReplayCheckpointTraceRayFloatBits(hitPos.x),
								ReplayCheckpointTraceRayFloatBits(hitPos.y),
								ReplayCheckpointTraceRayFloatBits(hitPos.z),
								f->pos.x, f->pos.y, f->pos.z,
								featureRelMid.x, featureRelMid.y, featureRelMid.z,
								cv.GetVolumeType(),
								cv.GetPrimaryAxis(),
								cv.GetScales().x, cv.GetScales().y, cv.GetScales().z,
								cv.GetOffsets().x, cv.GetOffsets().y, cv.GetOffsets().z,
								cv.IgnoreHits() ? 1u : 0u,
								cv.UseContHitTest() ? 1u : 0u,
								cv.DefaultToFootPrint() ? 1u : 0u,
								cv.DefaultToPieceTree() ? 1u : 0u);
						}

						// we want the closest feature (intersection point) on the ray
						if (len >= traceLength)
							continue;

						traceLength = len;

						hitFeature = f;
						*hitColQuery = cq;
					}
				}
			}
		}

		// unit intersection
		if (scanForAnyUnits) {
			for (const int quadIdx: *qfQuery.quads) {
				const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

				for (CUnit* u: quad.units) {
					if (u == owner)
						continue;

					if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
						continue;

					bool doHitTest = false;

					doHitTest |= (scanForAllies   && u->allyteam == owner->allyteam);
					doHitTest |= (scanForEnemies  && u->allyteam != owner->allyteam);
					doHitTest |= (scanForNeutrals && u->IsNeutral());
					doHitTest |= (scanForCloaked  && u->IsCloaked());

					if (!doHitTest)
						continue;

					const float traceLengthBefore = traceLength;
					if (CCollisionHandler::DetectHit(u, u->GetTransformMatrix(true), pos, traceEnd, &cq, true)) {
						const float len = cq.GetHitPosDist(pos, dir);
						const bool accepted = (len < traceLength);

						if (debugTraceRayFrame) {
							const CollisionVolume& cv = u->collisionVolume;
							const float3 hitPos = cq.GetHitPos();
							const CMatrix44f transform = u->GetTransformMatrix(true);
							const float3 unitFront = float3(u->frontdir.x, u->frontdir.y, u->frontdir.z);
							const float3 unitRight = float3(u->rightdir.x, u->rightdir.y, u->rightdir.z);
							const float3 unitUp = float3(u->updir.x, u->updir.y, u->updir.z);
							const float3 unitRelMid = float3(u->relMidPos.x, u->relMidPos.y, u->relMidPos.z);
							const float3 unitMid = float3(u->midPos.x, u->midPos.y, u->midPos.z);
							const float3 unitAim = float3(u->aimPos.x, u->aimPos.y, u->aimPos.z);

							LOG("[ReplayCheckpoint][traceray] unit-hit frame=%d owner=%d quad=%d candidate=%d accepted=%u prevLength=%.9g prevLengthBits=%08x len=%.9g lenBits=%08x pos=<%.9g,%.9g,%.9g> dir=<%.9g,%.9g,%.9g> hitPos=<%.9g,%.9g,%.9g> hitPosBits=<%08x,%08x,%08x> unitPos=<%.9g,%.9g,%.9g> unitFront=<%.9g,%.9g,%.9g> unitRight=<%.9g,%.9g,%.9g> unitUp=<%.9g,%.9g,%.9g> unitRelMid=<%.9g,%.9g,%.9g> unitMid=<%.9g,%.9g,%.9g> unitAim=<%.9g,%.9g,%.9g> heading=%d phys=%u coll=%u matrixX=<%.9g,%.9g,%.9g> matrixY=<%.9g,%.9g,%.9g> matrixZ=<%.9g,%.9g,%.9g> matrixPos=<%.9g,%.9g,%.9g> cvType=%d cvAxis=%d cvScales=<%.9g,%.9g,%.9g> cvHScales=<%.9g,%.9g,%.9g> cvOffsets=<%.9g,%.9g,%.9g> cvRadius=%.9g cvIgnore=%u cvCont=%u cvFoot=%u cvPiece=%u",
								gs->frameNum,
								owner != nullptr ? owner->id : -1,
								quadIdx,
								u->id,
								accepted ? 1u : 0u,
								traceLengthBefore,
								ReplayCheckpointTraceRayFloatBits(traceLengthBefore),
								len,
								ReplayCheckpointTraceRayFloatBits(len),
								pos.x, pos.y, pos.z,
								dir.x, dir.y, dir.z,
								hitPos.x, hitPos.y, hitPos.z,
								ReplayCheckpointTraceRayFloatBits(hitPos.x),
								ReplayCheckpointTraceRayFloatBits(hitPos.y),
								ReplayCheckpointTraceRayFloatBits(hitPos.z),
								u->pos.x, u->pos.y, u->pos.z,
								unitFront.x, unitFront.y, unitFront.z,
								unitRight.x, unitRight.y, unitRight.z,
								unitUp.x, unitUp.y, unitUp.z,
								unitRelMid.x, unitRelMid.y, unitRelMid.z,
								unitMid.x, unitMid.y, unitMid.z,
								unitAim.x, unitAim.y, unitAim.z,
								static_cast<int>(u->heading),
								static_cast<unsigned int>(u->physicalState),
								static_cast<unsigned int>(u->collidableState),
								transform.GetX().x, transform.GetX().y, transform.GetX().z,
								transform.GetY().x, transform.GetY().y, transform.GetY().z,
								transform.GetZ().x, transform.GetZ().y, transform.GetZ().z,
								transform.GetPos().x, transform.GetPos().y, transform.GetPos().z,
								cv.GetVolumeType(),
								cv.GetPrimaryAxis(),
								cv.GetScales().x, cv.GetScales().y, cv.GetScales().z,
								cv.GetHScales().x, cv.GetHScales().y, cv.GetHScales().z,
								cv.GetOffsets().x, cv.GetOffsets().y, cv.GetOffsets().z,
								cv.GetBoundingRadius(),
								cv.IgnoreHits() ? 1u : 0u,
								cv.UseContHitTest() ? 1u : 0u,
								cv.DefaultToFootPrint() ? 1u : 0u,
								cv.DefaultToPieceTree() ? 1u : 0u);
						}

						// we want the closest unit (intersection point) on the ray
						if (len >= traceLength)
							continue;

						traceLength = len;

						hitUnit = u;
						*hitColQuery = cq;
					}
				}
			}

			// units override features, so feature != null implies no unit was hit
			if (hitUnit != nullptr)
				hitFeature = nullptr;

		}
	}

	if (scanForGround) {
		// ground intersection
		const float groundLength = CGround::LineGroundCol(pos, pos + dir * traceLength);

		if (traceLength > groundLength && groundLength > 0.0f) {
			traceLength = groundLength;

			hitUnit = nullptr;
			hitFeature = nullptr;
		}
	}

	// no intersection if no decrease in length
	return traceLength;
}


void TraceRayShields(
	const CWeapon* emitter,
	const float3& start,
	const float3& dir,
	float length,
	std::vector<SShieldDist>& hitShields
) {
	RECOIL_DETAILED_TRACY_ZONE;
	CollisionQuery cq;

	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, start, dir, length);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		for (CPlasmaRepulser* r: quad.repulsers) {
			if (!r->CanIntercept(emitter->weaponDef->interceptedByShieldType, emitter->owner->allyteam))
				continue;

			if (CCollisionHandler::DetectHit(r->owner, &r->collisionVolume, r->owner->GetTransformMatrix(true), start, start + dir * length, &cq, true)) {
				if (cq.InsideHit() && r->weaponDef->exteriorShield)
					continue;

				const float len = cq.GetHitPosDist(start, dir);

				if (len <= 0.0f)
					continue;

				const auto hitCmp = [](const float a, const SShieldDist& b) { return (a < b.dist); };
				const auto insPos = std::upper_bound(hitShields.begin(), hitShields.end(), len, hitCmp);

				hitShields.insert(insPos, {r, len});
			}
		}
	}
}


float GuiTraceRay(
	const float3& start,
	const float3& dir,
	const float length,
	const CUnit* exclude,
	const CUnit*& hitUnit,
	const CFeature*& hitFeature,
	bool useRadar,
	bool groundOnly,
	bool ignoreWater
) {
	RECOIL_DETAILED_TRACY_ZONE;
	hitUnit = nullptr;
	hitFeature = nullptr;

	if (dir == ZeroVector)
		return -1.0f;

	// ground and water-plane intersection
	const float    guiRayLength = length;
	const float groundRayLength = CGround::LineGroundCol(start, dir, guiRayLength, false);
	const float  waterRayLength = CGround::LinePlaneCol(start, dir, guiRayLength, CGround::GetWaterPlaneLevel());

	float minRayLength = groundRayLength;
	float minIngressDist = length;
	float minEgressDist = length;

	bool hitFactory = false;

	// if ray cares about water, take minimum
	// of distance to ground and water surface
	if (!ignoreWater)
		minRayLength = std::min(groundRayLength, waterRayLength);
	if (groundOnly)
		return minRayLength;

	// set maximum ray until ground intersection taking lenience into account later
	float maxRayLength;
	if (minRayLength >= 0.0) {
		// normal intersection
		maxRayLength = minRayLength;
	} else if (waterRayLength >= 0.0) {
		// out of map we still want to intersect somewhere if possible
		maxRayLength = waterRayLength;
	} else {
		// pointing upwards
		maxRayLength = length;
	}
	maxRayLength = std::min(maxRayLength + globalConfig.selectThroughGround, length);

	CollisionQuery cq;

	QuadFieldQuery qfQuery;
	if (useRadar) {
		const float allyTeamError = losHandler->GetAllyTeamRadarErrorSize(gu->myAllyTeam);
		quadField.GetQuadsOnWideRay(qfQuery, start, dir, maxRayLength, allyTeamError);
	} else {
		quadField.GetQuadsOnRay(qfQuery, start, dir, maxRayLength);
	}

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		// Unit Intersection
		for (const CUnit* u: quad.units) {
			const bool unitIsEnemy = !teamHandler.Ally(u->allyteam, gu->myAllyTeam);
			const bool unitOnRadar = (useRadar && losHandler->InRadar(u, gu->myAllyTeam));
			const bool unitInSight = (u->losStatus[gu->myAllyTeam] & (LOS_INLOS | LOS_CONTRADAR));
			const bool unitVisible = !unitIsEnemy || unitOnRadar || unitInSight || gu->spectatingFullView;

			if (u == exclude)
				continue;
			#if 0
			// test this bit only in synced traces, rely on noSelect here
			if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
				continue;
			#endif
			if (u->noSelect)
				continue;
			if (!unitVisible)
				continue;

			CollisionVolume cv = u->selectionVolume;

			// for iconified units, just pretend the collision
			// volume is a sphere of radius <unit->IconRadius>
			// (count radar blips as such too)
			if (u->GetIsIcon() || (!unitInSight && unitOnRadar && unitIsEnemy))
				cv.InitSphere(u->iconRadius);

			if (CCollisionHandler::MouseHit(u, u->GetTransformMatrix(false), start, start + dir * guiRayLength, &cv, &cq)) {
				// get the distance to the ray-volume ingress point
				// (not likely to generate inside-hit special cases)
				const float ingressDist = cq.GetIngressPosDist(start, dir);
				const float  egressDist = cq.GetEgressPosDist(start, dir);

				const bool factoryUnderCursor = u->unitDef->IsFactoryUnit();
				const bool factoryHitBeforeUnit = ((hitFactory && ingressDist < minIngressDist) || (!hitFactory &&  egressDist < minIngressDist));
				const bool unitHitInsideFactory = ((hitFactory && ingressDist <  minEgressDist) || (!hitFactory && ingressDist < minIngressDist));

				// give units in a factory higher priority than the factory itself
				if (hitUnit == nullptr || (factoryUnderCursor && factoryHitBeforeUnit) || (!factoryUnderCursor && unitHitInsideFactory)) {
					hitFactory = factoryUnderCursor;
					minIngressDist = ingressDist;
					minEgressDist = egressDist;

					hitUnit = u;
					hitFeature = nullptr;
				}
			}
		}

		// Feature Intersection
		for (const CFeature* f: quad.features) {
			if (!gu->spectatingFullView && !f->IsInLosForAllyTeam(gu->myAllyTeam))
				continue;
			#if 0
			// test this bit only in synced traces, rely on noSelect here
			if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
				continue;
			#endif
			if (f->noSelect)
				continue;

			const CollisionVolume& cv = f->selectionVolume;

			if (CCollisionHandler::MouseHit(f, f->GetTransformMatrix(false), start, start + dir * guiRayLength, &cv, &cq)) {
				const float hitDist = cq.GetHitPosDist(start, dir);

				const bool factoryHitBeforeUnit = ( hitFactory && hitDist <  minEgressDist);
				const bool unitHitInsideFactory = (!hitFactory && hitDist < minIngressDist);

				// we want the closest feature (intersection point) on the ray
				// give features in a factory (?) higher priority than the factory itself
				if (hitUnit == nullptr || factoryHitBeforeUnit || unitHitInsideFactory) {
					hitFactory = false;
					minIngressDist = hitDist;

					hitFeature = f;
					hitUnit = nullptr;
				}
			}
		}
	}

	if ((minRayLength > 0.0f) && (maxRayLength < minIngressDist)) {
		minIngressDist = minRayLength;

		hitUnit    = nullptr;
		hitFeature = nullptr;
	}

	return minIngressDist;
}


bool TestCone(
	const float3& from,
	const float3& dir,
	float length,
	float spread,
	int allyteam,
	int traceFlags,
	CUnit* owner
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, from, dir, length);

	if (qfQuery.quads->empty())
		return true;

	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		if (scanForAllies) {
			for (const CUnit* u: quad.teamUnits[allyteam]) {
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, u))
					return true;
			}
		}

		if (scanForNeutrals) {
			for (const CUnit* u: quad.units) {
				if (!u->IsNeutral())
					continue;
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, u))
					return true;
			}
		}

		if (scanForFeatures) {
			for (const CFeature* f: quad.features) {
				if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestConeHelper(from, dir, length, spread, f))
					return true;
			}
		}
	}

	return false;
}



bool TestTrajectoryCone(
	const float3& from,
	const float3& dir,
	float length,
	float linear,
	float quadratic,
	float spread,
	int allyteam,
	int traceFlags,
	CUnit* owner
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetQuadsOnRay(qfQuery, from, dir, length);

	if (qfQuery.quads->empty())
		return true;

	const bool scanForAllies   = ((traceFlags & Collision::NOFRIENDLIES) == 0);
	const bool scanForNeutrals = ((traceFlags & Collision::NONEUTRALS  ) == 0);
	const bool scanForFeatures = ((traceFlags & Collision::NOFEATURES  ) == 0);

	for (const int quadIdx: *qfQuery.quads) {
		const CQuadField::Quad& quad = quadField.GetQuad(quadIdx);

		// friendly units in this quad
		if (scanForAllies) {
			for (const CUnit* u: quad.teamUnits[allyteam]) {
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, u))
					return true;

			}
		}

		// neutral units in this quad
		if (scanForNeutrals) {
			for (const CUnit* u: quad.units) {
				if (!u->IsNeutral())
					continue;
				if (u == owner)
					continue;
				if (!u->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, u))
					return true;
			}
		}

		// features in this quad
		if (scanForFeatures) {
			for (const CFeature* f: quad.features) {
				if (!f->HasCollidableStateBit(CSolidObject::CSTATE_BIT_QUADMAPRAYS))
					continue;

				if (TestTrajectoryConeHelper(from, dir, length, linear, quadratic, spread, 0.0f, f))
					return true;
			}
		}
	}

	return false;
}



} //namespace TraceRay
