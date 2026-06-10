/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "System/LoadSave/ReplayCheckpointHandler.h"

#include <string>

namespace ReplayCheckpointHandler
{
	struct SaveRequest {
		int frame = -1;
		std::string fileName;
		std::string saveArgs;

		bool IsValid() const { return !fileName.empty(); }
	};

	using CreateSaveCallback = bool (*)(const std::string& fileName, const std::string& saveArgs);
	using NormalizeDirectoryCallback = std::string (*)(const std::string& directory);

	std::string MakeSaveFileName(int frame);
	std::string MakeBundledSaveFileName(int frame, const std::string& bundleDir, NormalizeDirectoryCallback normalizeDirectory);
	SaveRequest MakeSaveRequest(int frame, bool overwrite, DemoContextMode mode, const std::string& bundleDir, NormalizeDirectoryCallback normalizeDirectory);
	bool ExecuteSaveRequest(const SaveRequest& request, CreateSaveCallback createSave);
}
