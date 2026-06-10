/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <vector>

#include "PathDefines.h"
#include "System/Misc/SpringTime.h"
#include "System/float3.h"
#include "System/type2.h"

class CSolidObject;

namespace QTPFS
{
	struct IPath;

	struct ReplayCheckpointPathNodeState {
		uint32_t nodeId = -1U;
		uint32_t nodeNumber = -1U;
		float2 netPoint;
		int pathPointIndex = -1;
		int xmin = 0;
		int zmin = 0;
		int xmax = 0;
		int zmax = 0;
		bool badNode = false;
	};

	struct ReplayCheckpointPathPayload {
		bool hasOwner = false;
		uint32_t ownerID = 0;
		uint32_t pathID = 0;
		uint32_t nextPointIndex = 0;
		uint32_t repathTriggerIndex = 0;
		uint32_t numPathUpdates = 0;
		uint32_t firstCleanNodeID = 0;
		PathHashType hash = BAD_HASH;
		PathHashType virtualHash = BAD_HASH;
		float radius = 0.0f;
		float3 boundingBoxMins;
		float3 boundingBoxMaxs;
		float3 goalPosition;
		spring_time searchTime;
		int pathType = 0;
		bool synced = true;
		bool fullPath = true;
		bool partialPath = false;
		bool rawPath = false;
		bool boundingBoxOverride = false;
		std::vector<float3> points;
		std::vector<ReplayCheckpointPathNodeState> nodes;
	};

	ReplayCheckpointPathPayload CaptureReplayCheckpointPathPayload(
		const IPath& path,
		uint32_t ownerID
	);
	void CaptureReplayCheckpointPathPayload(
		const IPath& path,
		uint32_t ownerID,
		ReplayCheckpointPathPayload& state
	);
	void ApplyReplayCheckpointPathPayload(
		const ReplayCheckpointPathPayload& state,
		IPath& path,
		const CSolidObject* owner
	);
}
