/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "ReplayCheckpointHandler.h"

#include "Game/Game.h"
#include "Net/GameServer.h"
#include "Sim/Misc/GlobalSynced.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileSystem.h"
#include "System/Log/ILog.h"
#include "System/StringUtil.h"

#include <cstdlib>
#include <limits>
#include <vector>
#include <utility>

namespace ReplayCheckpointHandler
{

std::string MakeSaveFileName(int frame)
{
	return "Saves/replaycheckpoint_" + IntToString(frame, "%06i") + ".ssf";
}

static bool TryParseCheckpointFrame(const std::string& path, int* frame)
{
	static const std::string prefix = "replaycheckpoint_";

	const std::string baseName = FileSystem::GetBasename(path);

	if (baseName.compare(0, prefix.size(), prefix) != 0)
		return false;

	const std::string frameText = baseName.substr(prefix.size());
	if (frameText.empty())
		return false;

	char* end = nullptr;
	const long parsedFrame = std::strtol(frameText.c_str(), &end, 10);

	if (end == frameText.c_str() || *end != '\0' || parsedFrame < 0 || parsedFrame > std::numeric_limits<int>::max())
		return false;

	*frame = static_cast<int>(parsedFrame);
	return true;
}

CheckpointFile FindNearestCheckpoint(int targetFrame)
{
	CheckpointFile nearest;

	if (targetFrame < 0)
		return nearest;

	const std::vector<std::string> checkpointFiles = dataDirsAccess.FindFiles("Saves", "replaycheckpoint_*.ssf");

	for (const std::string& path: checkpointFiles) {
		int frame = -1;

		if (!TryParseCheckpointFrame(path, &frame))
			continue;

		if (frame > targetFrame)
			continue;

		if (!nearest.IsValid() || frame > nearest.frame) {
			nearest.frame = frame;
			nearest.path = path;
		}
	}

	return nearest;
}

bool QueueSaveCurrentFrame(bool overwrite)
{
	if (game == nullptr || gs == nullptr)
		return false;

	const int frame = gs->frameNum;
	std::string fileName = MakeSaveFileName(frame);
	std::string saveArgs = overwrite ? "-y" : "";

	game->Save(std::move(fileName), std::move(saveArgs));
	LOG("[ReplayCheckpoint] queued checkpoint save for frame %d", frame);
	return true;
}

bool RequestHotLoadFrame(int targetFrame)
{
	if (targetFrame < 0)
		return false;

	const CheckpointFile checkpoint = FindNearestCheckpoint(targetFrame);

	if (!checkpoint.IsValid()) {
		LOG_L(L_WARNING,
			"[ReplayCheckpoint] no checkpoint found at or before requested frame %d",
			targetFrame
		);
		return false;
	}

	LOG("[ReplayCheckpoint] restore to frame %d resolved to checkpoint frame %d (%s)",
		targetFrame,
		checkpoint.frame,
		checkpoint.path.c_str()
	);

	if (gs != nullptr)
		gs->paused = true;

	if (gameServer != nullptr)
		gameServer->SetPaused(true);

	return game != nullptr && game->LoadReplayCheckpoint(checkpoint.path, checkpoint.frame, targetFrame);
}

}
