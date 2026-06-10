/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Sim/Path/QTPFS/PathReplayCheckpointState.h"
#include "Sim/Path/QTPFS/Path.h"
#include "Map/MapDimensions.h"

#include <catch_amalgamated.hpp>

MapDimensions mapDims;

namespace
{
void InitPathTestMapBounds()
{
	mapDims.mapx = 64;
	mapDims.mapy = 64;
	mapDims.Initialize();
}

QTPFS::ReplayCheckpointPathPayload MakePathPayload()
{
	QTPFS::ReplayCheckpointPathPayload state;
	state.hasOwner = true;
	state.ownerID = 123;
	state.pathID = 77;
	state.nextPointIndex = 2;
	state.repathTriggerIndex = 4;
	state.numPathUpdates = 5;
	state.firstCleanNodeID = 6;
	state.hash = QTPFS::PathHashType{0x1020304050607080ULL, 0x0102030405060708ULL};
	state.virtualHash = QTPFS::PathHashType{0x8877665544332211ULL, 0x1111222233334444ULL};
	state.radius = 24.5f;
	state.boundingBoxMins = float3(8.0f, 1.0f, 16.0f);
	state.boundingBoxMaxs = float3(96.0f, 3.0f, 128.0f);
	state.goalPosition = float3(144.0f, 4.0f, 160.0f);
	state.searchTime = spring_time::fromMilliSecs(321);
	state.pathType = 9;
	state.synced = false;
	state.fullPath = false;
	state.partialPath = true;
	state.rawPath = true;
	state.boundingBoxOverride = true;
	state.points = {
		float3(8.0f, 1.0f, 16.0f),
		float3(48.0f, 2.0f, 64.0f),
		float3(96.0f, 3.0f, 128.0f),
	};
	state.nodes = {
		{11, 21, float2(1.5f, 2.5f), 0, 1, 2, 3, 4, false},
		{12, 22, float2(3.5f, 4.5f), 1, 5, 6, 7, 8, true},
	};
	return state;
}

void CheckFloat3(const float3& value, const float3& expected)
{
	CHECK(value.x == Catch::Approx(expected.x));
	CHECK(value.y == Catch::Approx(expected.y));
	CHECK(value.z == Catch::Approx(expected.z));
}

void CheckPathMatchesPayload(
	const QTPFS::IPath& path,
	const QTPFS::ReplayCheckpointPathPayload& state,
	const CSolidObject* owner
) {
	CHECK(path.GetID() == state.pathID);
	CHECK(path.GetNextPointIndex() == state.nextPointIndex);
	CHECK(path.GetRepathTriggerIndex() == state.repathTriggerIndex);
	CHECK(path.GetNumPathUpdates() == state.numPathUpdates);
	CHECK(path.GetFirstNodeIdOfCleanPath() == state.firstCleanNodeID);
	CHECK(path.GetHash() == state.hash);
	CHECK(path.GetVirtualHash() == state.virtualHash);
	CHECK(path.GetRadius() == Catch::Approx(state.radius));
	CheckFloat3(path.GetBoundingBoxMins(), state.boundingBoxMins);
	CheckFloat3(path.GetBoundingBoxMaxs(), state.boundingBoxMaxs);
	CheckFloat3(path.GetGoalPosition(), state.goalPosition);
	CHECK(path.GetSearchTime().toNanoSecsi() == state.searchTime.toNanoSecsi());
	CHECK(path.GetPathType() == state.pathType);
	CHECK(path.IsSynced() == state.synced);
	CHECK(path.IsFullPath() == state.fullPath);
	CHECK(path.IsPartialPath() == state.partialPath);
	CHECK(path.IsRawPath() == state.rawPath);
	CHECK(path.IsBoundingBoxOverriden() == state.boundingBoxOverride);
	CHECK(path.GetOwner() == owner);
	REQUIRE(path.NumPoints() == state.points.size());
	for (unsigned int i = 0; i < path.NumPoints(); ++i) {
		CheckFloat3(path.GetPoint(i), state.points[i]);
	}
	REQUIRE(path.NumNodes() == state.nodes.size());
	for (unsigned int i = 0; i < path.NumNodes(); ++i) {
		const QTPFS::IPath::PathNodeData& node = path.GetNode(i);
		const QTPFS::ReplayCheckpointPathNodeState& expected = state.nodes[i];
		CHECK(node.nodeId == expected.nodeId);
		CHECK(node.nodeNumber == expected.nodeNumber);
		CHECK(node.netPoint.x == Catch::Approx(expected.netPoint.x));
		CHECK(node.netPoint.y == Catch::Approx(expected.netPoint.y));
		CHECK(node.pathPointIndex == expected.pathPointIndex);
		CHECK(node.xmin == expected.xmin);
		CHECK(node.zmin == expected.zmin);
		CHECK(node.xmax == expected.xmax);
		CHECK(node.zmax == expected.zmax);
		CHECK(node.badNode == expected.badNode);
	}
}
}

TEST_CASE("QTPFS replay checkpoint applies path payload")
{
	InitPathTestMapBounds();

	const auto state = MakePathPayload();
	const CSolidObject* owner = reinterpret_cast<const CSolidObject*>(0x1);

	QTPFS::IPath path;
	QTPFS::ApplyReplayCheckpointPathPayload(state, path, owner);

	CheckPathMatchesPayload(path, state, owner);
}

TEST_CASE("QTPFS replay checkpoint captures path payload")
{
	InitPathTestMapBounds();

	const auto state = MakePathPayload();
	const CSolidObject* owner = reinterpret_cast<const CSolidObject*>(0x1);

	QTPFS::IPath path;
	QTPFS::ApplyReplayCheckpointPathPayload(state, path, owner);
	const auto captured = QTPFS::CaptureReplayCheckpointPathPayload(path, state.ownerID);

	CHECK(captured.hasOwner);
	CHECK(captured.ownerID == state.ownerID);

	QTPFS::IPath restoredPath;
	QTPFS::ApplyReplayCheckpointPathPayload(captured, restoredPath, owner);

	CheckPathMatchesPayload(restoredPath, state, owner);
}
