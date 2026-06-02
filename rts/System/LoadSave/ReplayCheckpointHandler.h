/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef REPLAY_CHECKPOINT_HANDLER_H
#define REPLAY_CHECKPOINT_HANDLER_H

#include <string>

namespace ReplayCheckpointHandler
{
	struct CheckpointFile {
		int frame = -1;
		std::string path;

		bool IsValid() const { return (frame >= 0 && !path.empty()); }
	};

	std::string MakeSaveFileName(int frame);
	CheckpointFile FindNearestCheckpoint(int targetFrame);
	bool QueueSaveCurrentFrame(bool overwrite);
	bool RequestHotLoadFrame(int targetFrame);
}

#endif // REPLAY_CHECKPOINT_HANDLER_H
