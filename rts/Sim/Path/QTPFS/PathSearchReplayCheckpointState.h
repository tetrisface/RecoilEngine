/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>

namespace QTPFS
{
	using entity = std::int32_t;

	struct PathSearch;

	inline constexpr uint8_t REPLAY_CHECKPOINT_SEARCH_PATH = 1u;
	inline constexpr uint8_t REPLAY_CHECKPOINT_SEARCH_UNSYNCED = 2u;
	inline constexpr uint8_t REPLAY_CHECKPOINT_SEARCH_EXTERNAL = 3u;
	inline constexpr QTPFS::entity REPLAY_CHECKPOINT_INVALID_ENTITY = -1;

	struct ReplayCheckpointPathSearchState {
		QTPFS::entity entity = REPLAY_CHECKPOINT_INVALID_ENTITY;
		QTPFS::entity pathEntity = REPLAY_CHECKPOINT_INVALID_ENTITY;
		uint32_t searchType = 0;
		uint32_t searchTeam = 0;
		uint8_t componentKind = 0;
		int pathType = 0;
		bool processPath = false;
		bool rawPathCheck = false;
		bool synced = true;
		bool pathRequestWaiting = false;
		bool doPartialSearch = false;
		bool tryPathRepair = false;
		bool rejectPartialSearch = false;
		bool allowPartialSearch = false;
		bool expectIncompletePartialSearch = false;
		bool searchEarlyDrop = false;
		bool initialized = false;
		bool partialReverseTrace = false;
		bool doPathRepair = false;
		bool fwdPathConnected = false;
		bool bwdPathConnected = false;
		bool useFwdPathOnly = false;
		bool haveFullPath = false;
		bool havePartPath = false;
	};

	ReplayCheckpointPathSearchState CaptureReplayCheckpointPathSearchState(
		QTPFS::entity searchEntity,
		const PathSearch& search,
		uint8_t componentKind,
		bool processPath
	);
	void ApplyReplayCheckpointPathSearchState(
		const ReplayCheckpointPathSearchState& state,
		PathSearch& search,
		float goalDistance
	);
}
