/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/LoadSave/ReplayCheckpointSavePlanner.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

namespace
{
struct CapturedSave {
	std::string fileName;
	std::string saveArgs;
};

std::vector<CapturedSave> capturedSaves;
bool nextSaveResult = true;

bool CaptureCreateSave(const std::string& fileName, const std::string& saveArgs)
{
	capturedSaves.push_back({fileName, saveArgs});
	return nextSaveResult;
}

std::string EnsurePathSepAtEnd(const std::string& path)
{
	if (path.empty() || path.back() == '/')
		return path;

	return path + "/";
}

void ResetCapture(bool result = true)
{
	capturedSaves.clear();
	nextSaveResult = result;
}
}

TEST_CASE("Replay checkpoint save requests address the current frame")
{
	const auto request = ReplayCheckpointHandler::MakeSaveRequest(
		270,
		true,
		ReplayCheckpointHandler::DemoContextMode::None,
		"",
		EnsurePathSepAtEnd
	);

	CHECK(request.frame == 270);
	CHECK(request.fileName == "Saves/replaycheckpoint_000270.ssf");
	CHECK(request.saveArgs == "-y");
}

TEST_CASE("Replay checkpoint recording requests use the active bundle")
{
	const auto request = ReplayCheckpointHandler::MakeSaveRequest(
		360,
		false,
		ReplayCheckpointHandler::DemoContextMode::Recording,
		"cache/demo.replay-checkpoints",
		EnsurePathSepAtEnd
	);

	CHECK(request.frame == 360);
	CHECK(request.fileName == "cache/demo.replay-checkpoints/replaycheckpoint_000360.ssf");
	CHECK(request.saveArgs.empty());
}

TEST_CASE("Replay checkpoint save execution calls the save callback immediately")
{
	ResetCapture();

	const auto request = ReplayCheckpointHandler::MakeSaveRequest(
		180,
		true,
		ReplayCheckpointHandler::DemoContextMode::Recording,
		"bundle/",
		EnsurePathSepAtEnd
	);

	const bool saved = ReplayCheckpointHandler::ExecuteSaveRequest(request, CaptureCreateSave);

	REQUIRE(saved);
	REQUIRE(capturedSaves.size() == 1);
	CHECK(capturedSaves[0].fileName == "bundle/replaycheckpoint_000180.ssf");
	CHECK(capturedSaves[0].saveArgs == "-y");
}

TEST_CASE("Replay checkpoint save execution reports callback failure")
{
	ResetCapture(false);

	const auto request = ReplayCheckpointHandler::MakeSaveRequest(
		90,
		false,
		ReplayCheckpointHandler::DemoContextMode::None,
		"",
		EnsurePathSepAtEnd
	);

	const bool saved = ReplayCheckpointHandler::ExecuteSaveRequest(request, CaptureCreateSave);

	CHECK_FALSE(saved);
	REQUIRE(capturedSaves.size() == 1);
	CHECK(capturedSaves[0].fileName == "Saves/replaycheckpoint_000090.ssf");
	CHECK(capturedSaves[0].saveArgs.empty());
}
