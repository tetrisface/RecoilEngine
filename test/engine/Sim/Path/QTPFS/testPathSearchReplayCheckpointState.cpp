/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Sim/Path/QTPFS/PathSearchReplayCheckpointState.h"
#include "System/Misc/TracyDefs.h"
#include "Sim/Path/QTPFS/PathSearch.h"

#include <catch_amalgamated.hpp>

namespace
{
QTPFS::PathSearch MakeSearchWithReplayState()
{
	QTPFS::PathSearch search(17);
	search.SetID(42);
	search.SetTeam(3);
	search.SetPathType(5);
	search.rawPathCheck = true;
	search.synced = true;
	search.pathRequestWaiting = true;
	search.doPartialSearch = true;
	search.tryPathRepair = true;
	search.rejectPartialSearch = true;
	search.allowPartialSearch = true;
	search.expectIncompletePartialSearch = true;
	search.searchEarlyDrop = true;
	search.initialized = true;
	search.partialReverseTrace = true;
	search.doPathRepair = true;
	search.fwdPathConnected = true;
	search.bwdPathConnected = true;
	search.useFwdPathOnly = true;
	search.RestoreReplayCheckpointResultFlags(true, true);

	return search;
}
}

TEST_CASE("QTPFS replay checkpoint captures path search envelope")
{
	const QTPFS::PathSearch search = MakeSearchWithReplayState();
	const QTPFS::entity searchEntity = QTPFS::entity(77);

	const auto state = QTPFS::CaptureReplayCheckpointPathSearchState(
		searchEntity,
		search,
		QTPFS::REPLAY_CHECKPOINT_SEARCH_PATH,
		true
	);

	CHECK(state.entity == searchEntity);
	CHECK(state.pathEntity == QTPFS::entity(42));
	CHECK(state.searchType == 17);
	CHECK(state.searchTeam == 3);
	CHECK(state.componentKind == QTPFS::REPLAY_CHECKPOINT_SEARCH_PATH);
	CHECK(state.pathType == 5);
	CHECK(state.processPath);
	CHECK(state.rawPathCheck);
	CHECK(state.synced);
	CHECK(state.pathRequestWaiting);
	CHECK(state.doPartialSearch);
	CHECK(state.tryPathRepair);
	CHECK(state.rejectPartialSearch);
	CHECK(state.allowPartialSearch);
	CHECK(state.expectIncompletePartialSearch);
	CHECK(state.searchEarlyDrop);
	CHECK(state.initialized);
	CHECK(state.partialReverseTrace);
	CHECK(state.doPathRepair);
	CHECK(state.fwdPathConnected);
	CHECK(state.bwdPathConnected);
	CHECK(state.useFwdPathOnly);
	CHECK(state.haveFullPath);
	CHECK(state.havePartPath);
}

TEST_CASE("QTPFS replay checkpoint restores ready path search envelope")
{
	auto state = QTPFS::CaptureReplayCheckpointPathSearchState(
		QTPFS::entity(77),
		MakeSearchWithReplayState(),
		QTPFS::REPLAY_CHECKPOINT_SEARCH_UNSYNCED,
		true
	);

	QTPFS::PathSearch restoredSearch(state.searchType);
	QTPFS::ApplyReplayCheckpointPathSearchState(state, restoredSearch, 128.0f);

	CHECK(restoredSearch.GetID() == 42);
	CHECK(restoredSearch.GetTeam() == 3);
	CHECK(restoredSearch.GetSearchType() == 17);
	CHECK(restoredSearch.GetPathType() == 5);
	CHECK(restoredSearch.rawPathCheck);
	CHECK(restoredSearch.synced);
	CHECK(restoredSearch.pathRequestWaiting);
	CHECK(restoredSearch.doPartialSearch);
	CHECK(restoredSearch.tryPathRepair);
	CHECK(restoredSearch.rejectPartialSearch);
	CHECK(restoredSearch.allowPartialSearch);
	CHECK(restoredSearch.expectIncompletePartialSearch);
	CHECK(restoredSearch.searchEarlyDrop);
	CHECK(restoredSearch.initialized);
	CHECK(restoredSearch.partialReverseTrace);
	CHECK(restoredSearch.doPathRepair);
	CHECK(restoredSearch.fwdPathConnected);
	CHECK(restoredSearch.bwdPathConnected);
	CHECK(restoredSearch.useFwdPathOnly);
	CHECK(restoredSearch.HasFullPathResultForReplayCheckpoint());
	CHECK(restoredSearch.HasPartialPathResultForReplayCheckpoint());
}

TEST_CASE("QTPFS replay checkpoint only restores initialized state for process searches")
{
	QTPFS::ReplayCheckpointPathSearchState state;
	state.pathEntity = QTPFS::entity(42);
	state.initialized = true;
	state.processPath = false;

	QTPFS::PathSearch restoredSearch(0);
	restoredSearch.initialized = true;

	QTPFS::ApplyReplayCheckpointPathSearchState(state, restoredSearch, 128.0f);

	CHECK_FALSE(restoredSearch.initialized);
}
