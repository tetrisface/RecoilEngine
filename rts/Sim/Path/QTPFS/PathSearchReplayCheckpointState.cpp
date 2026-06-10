/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "PathSearchReplayCheckpointState.h"
#include "System/Misc/TracyDefs.h"
#include "PathSearch.h"

namespace QTPFS
{
ReplayCheckpointPathSearchState CaptureReplayCheckpointPathSearchState(
	QTPFS::entity searchEntity,
	const PathSearch& search,
	uint8_t componentKind,
	bool processPath
) {
	ReplayCheckpointPathSearchState state;
	state.entity = searchEntity;
	state.pathEntity = QTPFS::entity(search.GetID());
	state.searchType = search.GetSearchType();
	state.searchTeam = search.GetTeam();
	state.componentKind = componentKind;
	state.pathType = search.GetPathType();
	state.processPath = processPath;
	state.rawPathCheck = search.rawPathCheck;
	state.synced = search.synced;
	state.pathRequestWaiting = search.pathRequestWaiting;
	state.doPartialSearch = search.doPartialSearch;
	state.tryPathRepair = search.tryPathRepair;
	state.rejectPartialSearch = search.rejectPartialSearch;
	state.allowPartialSearch = search.allowPartialSearch;
	state.expectIncompletePartialSearch = search.expectIncompletePartialSearch;
	state.searchEarlyDrop = search.searchEarlyDrop;
	state.initialized = search.initialized;
	state.partialReverseTrace = search.partialReverseTrace;
	state.doPathRepair = search.doPathRepair;
	state.fwdPathConnected = search.fwdPathConnected;
	state.bwdPathConnected = search.bwdPathConnected;
	state.useFwdPathOnly = search.useFwdPathOnly;
	state.haveFullPath = search.HasFullPathResultForReplayCheckpoint();
	state.havePartPath = search.HasPartialPathResultForReplayCheckpoint();

	return state;
}

void ApplyReplayCheckpointPathSearchState(
	const ReplayCheckpointPathSearchState& state,
	PathSearch& search,
	float goalDistance
) {
	search.SetID(static_cast<unsigned int>(state.pathEntity));
	search.SetTeam(state.searchTeam);
	search.SetPathType(state.pathType);
	search.SetGoalDistance(goalDistance);
	search.rawPathCheck = state.rawPathCheck;
	search.synced = state.synced;
	search.pathRequestWaiting = state.pathRequestWaiting;
	search.doPartialSearch = state.doPartialSearch;
	search.tryPathRepair = state.tryPathRepair;
	search.rejectPartialSearch = state.rejectPartialSearch;
	search.allowPartialSearch = state.allowPartialSearch;
	search.expectIncompletePartialSearch = state.expectIncompletePartialSearch;
	search.searchEarlyDrop = state.searchEarlyDrop;
	search.initialized = state.processPath && state.initialized;
	search.partialReverseTrace = state.partialReverseTrace;
	search.doPathRepair = state.doPathRepair;
	search.fwdPathConnected = state.fwdPathConnected;
	search.bwdPathConnected = state.bwdPathConnected;
	search.useFwdPathOnly = state.useFwdPathOnly;
	search.RestoreReplayCheckpointResultFlags(state.haveFullPath, state.havePartPath);
}
}
