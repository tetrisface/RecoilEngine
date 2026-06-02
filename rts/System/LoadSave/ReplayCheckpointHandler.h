/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef REPLAY_CHECKPOINT_HANDLER_H
#define REPLAY_CHECKPOINT_HANDLER_H

#include <string>

namespace ReplayCheckpointHandler
{
	enum class DemoContextMode {
		None,
		Recording,
		Playback,
	};

	struct CheckpointFile {
		int frame = -1;
		std::string path;

		bool IsValid() const { return (frame >= 0 && !path.empty()); }
	};

	std::string GetBundleDirForDemo(const std::string& demoPath);
	std::string MakeSaveFileName(int frame);
	std::string MakeBundledSaveFileName(int frame, const std::string& bundleDir);

	void InitPlaybackContext(const std::string& demoPath);
	void UpdateRecordingContext();
	void ClearActiveContext();

	CheckpointFile FindNearestCheckpoint(int targetFrame);
	bool QueueSaveCurrentFrame(bool overwrite);
	bool RequestHotLoadFrame(int targetFrame);

	void UpdateRecordFrame(int frame);
	void NotifySaveCompleted(const std::string& savePath);
	void NotifySaveFailed();
	void FinalizeRecordingBundle();
}

#endif // REPLAY_CHECKPOINT_HANDLER_H
