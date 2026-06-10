/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "PathReplayCheckpointState.h"
#include "Path.h"

namespace QTPFS
{
ReplayCheckpointPathPayload CaptureReplayCheckpointPathPayload(
	const IPath& path,
	uint32_t ownerID
) {
	ReplayCheckpointPathPayload state;
	CaptureReplayCheckpointPathPayload(path, ownerID, state);
	return state;
}

void CaptureReplayCheckpointPathPayload(
	const IPath& path,
	uint32_t ownerID,
	ReplayCheckpointPathPayload& state
) {
	state.hasOwner = (path.GetOwner() != nullptr);
	state.ownerID = state.hasOwner ? ownerID : 0u;
	state.pathID = path.GetID();
	state.nextPointIndex = path.GetNextPointIndex();
	state.repathTriggerIndex = path.GetRepathTriggerIndex();
	state.numPathUpdates = path.GetNumPathUpdates();
	state.firstCleanNodeID = path.GetFirstNodeIdOfCleanPath();
	state.hash = path.GetHash();
	state.virtualHash = path.GetVirtualHash();
	state.radius = path.GetRadius();
	state.boundingBoxMins = path.GetBoundingBoxMins();
	state.boundingBoxMaxs = path.GetBoundingBoxMaxs();
	state.goalPosition = path.GetGoalPosition();
	state.searchTime = path.GetSearchTime();
	state.pathType = path.GetPathType();
	state.synced = path.IsSynced();
	state.fullPath = path.IsFullPath();
	state.partialPath = path.IsPartialPath();
	state.rawPath = path.IsRawPath();
	state.boundingBoxOverride = path.IsBoundingBoxOverriden();

	state.points.clear();
	state.points.reserve(path.NumPoints());
	for (unsigned int i = 0; i < path.NumPoints(); ++i) {
		state.points.push_back(path.GetPoint(i));
	}

	state.nodes.clear();
	const unsigned int nodeCount = path.NumNodes();
	state.nodes.reserve(nodeCount);
	for (unsigned int i = 0; i < nodeCount; ++i) {
		const IPath::PathNodeData& node = path.GetNode(i);
		state.nodes.push_back({
			node.nodeId,
			node.nodeNumber,
			node.netPoint,
			node.pathPointIndex,
			node.xmin,
			node.zmin,
			node.xmax,
			node.zmax,
			node.badNode
		});
	}
}

void ApplyReplayCheckpointPathPayload(
	const ReplayCheckpointPathPayload& state,
	IPath& path,
	const CSolidObject* owner
) {
	path.SetID(state.pathID);
	path.RestorePathTypeForReplayCheckpoint(state.pathType);
	path.SetNextPointIndex(state.nextPointIndex);
	path.SetRepathTriggerIndex(state.repathTriggerIndex);
	path.SetNumPathUpdates(state.numPathUpdates);
	path.SetFirstNodeIdOfCleanPath(state.firstCleanNodeID);
	path.SetHash(state.hash);
	path.SetVirtualHash(state.virtualHash);
	path.SetRadius(state.radius);
	path.SetSynced(state.synced);
	path.SetHasFullPath(state.fullPath);
	path.SetHasPartialPath(state.partialPath);
	path.SetIsRawPath(state.rawPath);
	path.AllocPoints(state.points.size());
	for (unsigned int i = 0; i < state.points.size(); ++i) {
		path.SetPoint(i, state.points[i]);
	}
	path.AllocNodes(state.nodes.size());
	for (unsigned int i = 0; i < state.nodes.size(); ++i) {
		const ReplayCheckpointPathNodeState& node = state.nodes[i];
		path.SetNode(i, node.nodeId, node.nodeNumber, float2{node.netPoint.x, node.netPoint.y}, node.pathPointIndex, node.badNode);
		path.SetNodeBoundary(i, node.xmin, node.zmin, node.xmax, node.zmax);
	}
	path.SetOwner(state.hasOwner ? owner : nullptr);
	path.SetGoalPosition(state.goalPosition);
	path.SetSearchTime(state.searchTime);
	path.RestoreBoundingBoxState(state.boundingBoxMins, state.boundingBoxMaxs, state.boundingBoxOverride);
}
}
