/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

namespace FeatureUpdateQueuePolicy
{
	struct FeatureState {
		bool deleteMe = false;
		bool moveControlEnabled = false;
		bool hasVelocity = false;
		bool hasSmokeOrFire = false;
		bool geoThermal = false;
		bool onGround = true;
		int creationFrame = -1;
	};

	bool CreationFrameNotificationPending(int creationFrame, int restoredFrame);
	bool NeedsUpdateAfterLoad(const FeatureState& feature, int restoredFrame);
}
