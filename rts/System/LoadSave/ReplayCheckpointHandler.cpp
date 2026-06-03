/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "ReplayCheckpointHandler.h"

#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/GameVersion.h"
#include "Net/GameServer.h"
#include "Net/Protocol/NetProtocol.h"
#include "Sim/Misc/GlobalSynced.h"
#include "System/Config/ConfigHandler.h"
#include "System/CRC.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileQueryFlags.h"
#include "System/FileSystem/FileSystem.h"
#include "System/LoadSave/DemoRecorder.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/StringUtil.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

CONFIG(bool, ReplayCheckpointAutoRecord).defaultValue(false);
CONFIG(int, ReplayCheckpointRecordInterval).defaultValue(90).minimumValue(1);
CONFIG(int, ReplayCheckpointMaxCount).defaultValue(120).minimumValue(1);


namespace ReplayCheckpointHandler
{

static constexpr const char* BUNDLE_SUFFIX = ".replay-checkpoints";
static constexpr const char* BUNDLE_PREFIX = "rcp_";
static constexpr const char* SESSION_FILE = "session.json";

struct RecordedCheckpoint {
	int frame = -1;
	std::string saveFile;
	std::string path;
};

struct DemoContext {
	DemoContextMode mode = DemoContextMode::None;
	std::string demoPath;
	std::string bundleDir;
};

static DemoContext activeContext;
static std::vector<RecordedCheckpoint> recordedCheckpoints;
static std::atomic<bool> saveInFlight = {false};
static int lastAutoSaveFrame = -1;
static spring_time lastAutoSaveWallTime = spring_gettime();
static bool sessionFinalized = false;


static std::string ResolveDemoPath(const std::string& demoPath)
{
	if (demoPath.empty())
		return "";

	if (FileSystem::FileExists(demoPath))
		return FileSystem::GetNormalizedPath(demoPath);

	return dataDirsAccess.LocateFile(demoPath);
}

static std::string ResolveBundleDir(const std::string& demoPath)
{
	const std::string resolvedDemo = ResolveDemoPath(demoPath);
	if (resolvedDemo.empty())
		return "";

	const std::string demoDir = FileSystem::GetDirectory(resolvedDemo);
	const std::string demoFile = FileSystem::GetFilename(resolvedDemo);
	if (demoFile.empty())
		return resolvedDemo + BUNDLE_SUFFIX;

	const uint32_t demoNameDigest = CRC::CalcDigest(demoFile.data(), demoFile.size());
	const std::string bundleName =
		std::string(BUNDLE_PREFIX) +
		IntToString(static_cast<int>(demoNameDigest), "%08x") +
		BUNDLE_SUFFIX;

	if (demoDir.empty())
		return bundleName;

	return FileSystem::EnsurePathSepAtEnd(demoDir) + bundleName;
}

static std::string GetRecordingDemoPath()
{
	if (clientNet != nullptr) {
		CDemoRecorder* demoRecorder = clientNet->GetDemoRecorder();
		if (demoRecorder != nullptr && demoRecorder->IsValid())
			return demoRecorder->GetName();
	}

	if (gameServer != nullptr) {
		const std::unique_ptr<CDemoRecorder>& demoRecorder = gameServer->GetDemoRecorder();
		if (demoRecorder != nullptr && demoRecorder->IsValid())
			return demoRecorder->GetName();
	}

	return "";
}

static bool IsDemoRecordingActive()
{
	return !GetRecordingDemoPath().empty();
}

static std::string JsonEscape(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size());

	for (char ch: value) {
		switch (ch) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			default: escaped += ch; break;
		}
	}

	return escaped;
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

static std::vector<std::string> ListCheckpointFiles(const std::string& bundleDir)
{
	if (bundleDir.empty() || !FileSystem::DirExists(bundleDir))
		return {};

	return dataDirsAccess.FindFiles(
		FileSystem::EnsurePathSepAtEnd(bundleDir),
		"replaycheckpoint_*.ssf",
		0
	);
}

static void PruneOldCheckpoints()
{
	const int maxCount = configHandler->GetInt("ReplayCheckpointMaxCount");
	if (maxCount <= 0 || static_cast<int>(recordedCheckpoints.size()) <= maxCount)
		return;

	std::sort(recordedCheckpoints.begin(), recordedCheckpoints.end(), [](const RecordedCheckpoint& a, const RecordedCheckpoint& b) {
		return a.frame < b.frame;
	});

	while (static_cast<int>(recordedCheckpoints.size()) > maxCount) {
		const RecordedCheckpoint oldest = recordedCheckpoints.front();
		recordedCheckpoints.erase(recordedCheckpoints.begin());

		if (!oldest.path.empty())
			FileSystem::Remove(oldest.path);

		LOG("[ReplayCheckpoint] pruned checkpoint frame %d from bundle", oldest.frame);
	}
}


std::string GetBundleDirForDemo(const std::string& demoPath)
{
	return ResolveBundleDir(demoPath);
}

std::string MakeSaveFileName(int frame)
{
	return "Saves/replaycheckpoint_" + IntToString(frame, "%06i") + ".ssf";
}

std::string MakeBundledSaveFileName(int frame, const std::string& bundleDir)
{
	return FileSystem::EnsurePathSepAtEnd(bundleDir) + "replaycheckpoint_" + IntToString(frame, "%06i") + ".ssf";
}

void InitPlaybackContext(const std::string& demoPath)
{
	ClearActiveContext();

	const std::string bundleDir = ResolveBundleDir(demoPath);
	if (bundleDir.empty()) {
		LOG_L(L_WARNING, "[ReplayCheckpoint] no checkpoint bundle for demo %s", demoPath.c_str());
		return;
	}

	activeContext.mode = DemoContextMode::Playback;
	activeContext.demoPath = demoPath;
	activeContext.bundleDir = bundleDir;

	if (!FileSystem::DirExists(bundleDir)) {
		LOG_L(L_WARNING, "[ReplayCheckpoint] no checkpoint bundle for demo %s", demoPath.c_str());
		return;
	}

	LOG("[ReplayCheckpoint] playback bundle %s", bundleDir.c_str());
}

void UpdateRecordingContext()
{
	if (gameSetup != nullptr && gameSetup->hostDemo)
		return;

	if (!configHandler->GetBool("ReplayCheckpointAutoRecord"))
		return;

	const std::string demoPath = GetRecordingDemoPath();
	if (demoPath.empty())
		return;

	const std::string bundleDir = ResolveBundleDir(demoPath);
	if (bundleDir.empty())
		return;

	if (!FileSystem::CreateDirectory(bundleDir))
		return;

	if (activeContext.mode == DemoContextMode::Recording && activeContext.demoPath == demoPath)
		return;

	activeContext.mode = DemoContextMode::Recording;
	activeContext.demoPath = demoPath;
	activeContext.bundleDir = bundleDir;
	recordedCheckpoints.clear();
	sessionFinalized = false;
	lastAutoSaveFrame = -1;
	lastAutoSaveWallTime = spring_gettime();

	LOG("[ReplayCheckpoint] recording bundle %s", bundleDir.c_str());
}

void ClearActiveContext()
{
	activeContext = DemoContext();
	recordedCheckpoints.clear();
	saveInFlight = false;
	lastAutoSaveFrame = -1;
	sessionFinalized = false;
}

CheckpointFile FindNearestCheckpoint(int targetFrame)
{
	CheckpointFile nearest;

	if (targetFrame < 0)
		return nearest;

	std::vector<std::string> checkpointFiles;

	if (activeContext.mode == DemoContextMode::Playback || activeContext.mode == DemoContextMode::Recording) {
		if (activeContext.bundleDir.empty())
			return nearest;

		checkpointFiles = ListCheckpointFiles(activeContext.bundleDir);
	} else {
		checkpointFiles = dataDirsAccess.FindFiles("Saves", "replaycheckpoint_*.ssf");
	}

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

	if (saveInFlight.load())
		return false;

	const int frame = gs->frameNum;
	std::string fileName;

	if (activeContext.mode == DemoContextMode::Recording && !activeContext.bundleDir.empty()) {
		fileName = MakeBundledSaveFileName(frame, activeContext.bundleDir);
	} else {
		fileName = MakeSaveFileName(frame);
	}

	std::string saveArgs = overwrite ? "-y" : "";

	saveInFlight = true;
	game->Save(std::move(fileName), std::move(saveArgs));
	LOG("[ReplayCheckpoint] queued checkpoint save for frame %d", frame);
	return true;
}

bool RequestHotLoadFrame(int targetFrame)
{
	if (targetFrame < 0)
		return false;

	if (activeContext.mode != DemoContextMode::Playback && gameSetup != nullptr && gameSetup->hostDemo)
		InitPlaybackContext(gameSetup->demoName);

	if (activeContext.mode == DemoContextMode::Playback && !FileSystem::DirExists(activeContext.bundleDir)) {
		LOG_L(L_WARNING,
			"[ReplayCheckpoint] no checkpoint bundle for demo %s",
			activeContext.demoPath.c_str()
		);
		return false;
	}

	const CheckpointFile checkpoint = FindNearestCheckpoint(targetFrame);

	if (!checkpoint.IsValid()) {
		if (activeContext.mode == DemoContextMode::Playback) {
			LOG_L(L_WARNING,
				"[ReplayCheckpoint] no checkpoint found at or before requested frame %d in bundle for demo %s",
				targetFrame,
				activeContext.demoPath.c_str()
			);
		} else {
			LOG_L(L_WARNING,
				"[ReplayCheckpoint] no checkpoint found at or before requested frame %d",
				targetFrame
			);
		}
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

void UpdateRecordFrame(int frame)
{
	UpdateRecordingContext();

	if (activeContext.mode != DemoContextMode::Recording)
		return;

	if (!configHandler->GetBool("ReplayCheckpointAutoRecord"))
		return;

	if (gameSetup != nullptr && gameSetup->hostDemo)
		return;

	if (!IsDemoRecordingActive())
		return;

	const int interval = configHandler->GetInt("ReplayCheckpointRecordInterval");
	if (interval <= 0 || frame <= 0 || (frame % interval) != 0)
		return;

	if (saveInFlight.load())
		return;

	const spring_time now = spring_gettime();
	if ((now - lastAutoSaveWallTime).toSecsf() < 1.0f)
		return;

	if (frame == lastAutoSaveFrame)
		return;

	lastAutoSaveFrame = frame;
	lastAutoSaveWallTime = now;
	QueueSaveCurrentFrame(true);
}

void NotifySaveFailed()
{
	saveInFlight = false;
}

void NotifySaveCompleted(const std::string& savePath)
{
	saveInFlight = false;

	if (activeContext.mode != DemoContextMode::Recording)
		return;

	if (savePath.find(BUNDLE_SUFFIX) == std::string::npos)
		return;

	int frame = -1;
	if (!TryParseCheckpointFrame(savePath, &frame))
		return;

	RecordedCheckpoint checkpoint;
	checkpoint.frame = frame;
	checkpoint.path = savePath;
	checkpoint.saveFile = FileSystem::GetFilename(savePath);

	auto existing = std::find_if(recordedCheckpoints.begin(), recordedCheckpoints.end(), [frame](const RecordedCheckpoint& cp) {
		return cp.frame == frame;
	});
	if (existing != recordedCheckpoints.end())
		*existing = checkpoint;
	else
		recordedCheckpoints.push_back(checkpoint);

	PruneOldCheckpoints();
	LOG("[ReplayCheckpoint] recorded checkpoint frame %d to %s", frame, savePath.c_str());
}

void FinalizeRecordingBundle()
{
	if (sessionFinalized)
		return;

	if (activeContext.mode != DemoContextMode::Recording || activeContext.bundleDir.empty())
		return;

	if (recordedCheckpoints.empty()) {
		LOG_L(L_WARNING, "[ReplayCheckpoint] no checkpoints recorded for demo %s", activeContext.demoPath.c_str());
		return;
	}

	std::sort(recordedCheckpoints.begin(), recordedCheckpoints.end(), [](const RecordedCheckpoint& a, const RecordedCheckpoint& b) {
		return a.frame < b.frame;
	});

	const std::string sessionPath = FileSystem::EnsurePathSepAtEnd(activeContext.bundleDir) + SESSION_FILE;
	std::ofstream out(sessionPath, std::ios::out | std::ios::trunc);
	if (!out.good()) {
		LOG_L(L_ERROR, "[ReplayCheckpoint] failed to write session manifest %s", sessionPath.c_str());
		return;
	}

	const std::string engineVersion = SpringVersion::GetSync();
	const std::string mapName = (gameSetup != nullptr) ? gameSetup->mapName : "";
	const std::string modName = (gameSetup != nullptr) ? gameSetup->modName : "";
	const std::string gameId = (gameSetup != nullptr) ? gameSetup->gameID : "";

	out << "{\n";
	out << "  \"schema_version\": 1,\n";
	out << "  \"demo_file\": \"" << JsonEscape(activeContext.demoPath) << "\",\n";
	out << "  \"game_id\": \"" << JsonEscape(gameId) << "\",\n";
	out << "  \"engine_version\": \"" << JsonEscape(engineVersion) << "\",\n";
	out << "  \"map_name\": \"" << JsonEscape(mapName) << "\",\n";
	out << "  \"mod_name\": \"" << JsonEscape(modName) << "\",\n";
	out << "  \"checkpoints\": [\n";

	for (size_t i = 0; i < recordedCheckpoints.size(); ++i) {
		const RecordedCheckpoint& checkpoint = recordedCheckpoints[i];
		out << "    {\"frame\": " << checkpoint.frame
		    << ", \"save_file\": \"" << JsonEscape(checkpoint.saveFile) << "\"}";
		if (i + 1 < recordedCheckpoints.size())
			out << ",";
		out << "\n";
	}

	out << "  ]\n";
	out << "}\n";

	sessionFinalized = true;
	LOG("[ReplayCheckpoint] wrote session manifest %s (%u checkpoints)",
		sessionPath.c_str(),
		static_cast<unsigned>(recordedCheckpoints.size())
	);
}

}
