/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "ReplayCheckpointSavePlanner.h"

#include <iomanip>
#include <sstream>

namespace ReplayCheckpointHandler
{
static std::string FormatCheckpointFrame(int frame)
{
	std::ostringstream stream;
	stream << std::setfill('0') << std::setw(6) << frame;
	return stream.str();
}

std::string MakeSaveFileName(int frame)
{
	return "Saves/replaycheckpoint_" + FormatCheckpointFrame(frame) + ".ssf";
}

std::string MakeBundledSaveFileName(int frame, const std::string& bundleDir, NormalizeDirectoryCallback normalizeDirectory)
{
	if (normalizeDirectory == nullptr)
		return "";

	return normalizeDirectory(bundleDir) + "replaycheckpoint_" + FormatCheckpointFrame(frame) + ".ssf";
}

SaveRequest MakeSaveRequest(int frame, bool overwrite, DemoContextMode mode, const std::string& bundleDir, NormalizeDirectoryCallback normalizeDirectory)
{
	SaveRequest request;
	request.frame = frame;
	request.saveArgs = overwrite ? "-y" : "";

	if (mode == DemoContextMode::Recording && !bundleDir.empty())
		request.fileName = MakeBundledSaveFileName(frame, bundleDir, normalizeDirectory);
	else
		request.fileName = MakeSaveFileName(frame);

	return request;
}

bool ExecuteSaveRequest(const SaveRequest& request, CreateSaveCallback createSave)
{
	if (!request.IsValid() || createSave == nullptr)
		return false;

	return createSave(request.fileName, request.saveArgs);
}
}
