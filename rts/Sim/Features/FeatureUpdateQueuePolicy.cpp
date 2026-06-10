/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FeatureUpdateQueuePolicy.h"

namespace FeatureUpdateQueuePolicy
{
bool CreationFrameNotificationPending(int creationFrame, int restoredFrame)
{
	if (restoredFrame < 0)
		return false;

	// Checkpoints are captured after frame N and resume by incrementing to N+1.
	return creationFrame >= restoredFrame && creationFrame <= (restoredFrame + 1);
}

bool NeedsUpdateAfterLoad(const FeatureState& feature, int restoredFrame)
{
	if (feature.deleteMe)
		return true;

	if (feature.moveControlEnabled)
		return true;

	if (feature.hasVelocity)
		return true;

	if (feature.hasSmokeOrFire)
		return true;

	if (feature.geoThermal)
		return true;

	if (CreationFrameNotificationPending(feature.creationFrame, restoredFrame))
		return true;

	return !feature.onGround;
}
}
