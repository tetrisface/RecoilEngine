/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Sim/Features/FeatureUpdateQueuePolicy.h"

#include <catch_amalgamated.hpp>

namespace
{
FeatureUpdateQueuePolicy::FeatureState StationaryGroundFeature(int creationFrame = 0)
{
	FeatureUpdateQueuePolicy::FeatureState feature;
	feature.creationFrame = creationFrame;
	return feature;
}
}

TEST_CASE("Feature update queue policy keeps checkpoint-boundary creation notifications")
{
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(StationaryGroundFeature(90), 90));
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(StationaryGroundFeature(91), 90));
	CHECK_FALSE(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(StationaryGroundFeature(89), 90));
	CHECK_FALSE(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(StationaryGroundFeature(92), 90));
}

TEST_CASE("Feature update queue policy ignores creation-frame window without a restore frame")
{
	CHECK_FALSE(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(StationaryGroundFeature(90), -1));
}

TEST_CASE("Feature update queue policy preserves dynamic feature updates after load")
{
	auto feature = StationaryGroundFeature(10);

	feature.deleteMe = true;
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(feature, 90));
	feature.deleteMe = false;

	feature.moveControlEnabled = true;
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(feature, 90));
	feature.moveControlEnabled = false;

	feature.hasVelocity = true;
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(feature, 90));
	feature.hasVelocity = false;

	feature.hasSmokeOrFire = true;
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(feature, 90));
	feature.hasSmokeOrFire = false;

	feature.geoThermal = true;
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(feature, 90));
	feature.geoThermal = false;

	feature.onGround = false;
	CHECK(FeatureUpdateQueuePolicy::NeedsUpdateAfterLoad(feature, 90));
}
