#include "CobFileHandler.h"
#include "System/FileSystem/FileHandler.h"
#include "System/Log/ILog.h"
#include "System/SpringHash.h"

#include <algorithm>

#include "System/Misc/TracyDefs.h"

namespace {
	bool IsReplayCheckpointDebugCobFile(const std::string& name)
	{
		return (
			name.find("CORCOM.cob") != std::string::npos ||
			name.find("corcom.cob") != std::string::npos
		);
	}

	uint32_t GetCobCodeChecksum(const CCobFile* file)
	{
		uint32_t checksum = 0u;

		if (file == nullptr)
			return checksum;

		for (const int opCode: file->code) {
			checksum = spring::LiteHash(opCode, checksum);
		}

		return checksum;
	}

	void LogReplayCheckpointDebugCobFile(const char* action, const std::string& requestName, const CCobFile* file, bool cached)
	{
		if (!IsReplayCheckpointDebugCobFile(requestName))
			return;

		LOG("[ReplayCheckpoint][COB] %s request=\"%s\" cached=%d file=%p file-name=\"%s\" code-words=%u code-cs=%u scripts=%u pieces=%u",
			action,
			requestName.c_str(),
			cached ? 1 : 0,
			static_cast<const void*>(file),
			(file != nullptr) ? file->name.c_str() : "",
			(file != nullptr) ? static_cast<unsigned int>(file->code.size()) : 0u,
			GetCobCodeChecksum(file),
			(file != nullptr) ? static_cast<unsigned int>(file->scriptNames.size()) : 0u,
			(file != nullptr) ? static_cast<unsigned int>(file->pieceNames.size()) : 0u
		);
	}
}

CCobFile* CCobFileHandler::GetCobFile(const std::string& name)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto it = cobFileHandles.find(name);

	if (it != cobFileHandles.end()) {
		CCobFile* file = &cobFileObjects[it->second];
		LogReplayCheckpointDebugCobFile("hit", name, file, true);
		return file;
	}

	CFileHandler f(name);

	if (!f.FileExists()) {
		LogReplayCheckpointDebugCobFile("missing", name, nullptr, false);
		return nullptr;
	}

	cobFileHandles[name] = cobFileObjects.size();
	cobFileObjects.emplace_back(CCobFile(f, name));

	CCobFile* file = &cobFileObjects[cobFileObjects.size() - 1];
	LogReplayCheckpointDebugCobFile("load", name, file, false);
	return file;
}


CCobFile* CCobFileHandler::ReloadCobFile(const std::string& name)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto it = cobFileHandles.find(name);

	if (it == cobFileHandles.end())
		return (GetCobFile(name));

	CFileHandler f(name);
	assert(f.FileExists());

	cobFileObjects[it->second] = CCobFile(f, name);
	return &cobFileObjects[it->second];
}


const CCobFile* CCobFileHandler::GetScriptFile(const std::string& name) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto it = cobFileHandles.find(name);

	if (it != cobFileHandles.end())
		return &cobFileObjects[it->second];

	return nullptr;
}

void CCobFileHandler::SaveMutableCode(std::vector<std::string>& names, std::vector<std::vector<int>>& codes) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	names.clear();
	codes.clear();
	names.reserve(cobFileObjects.size());
	codes.reserve(cobFileObjects.size());

	for (const CCobFile& file: cobFileObjects) {
		names.push_back(file.name);
		codes.push_back(file.code);
	}
}

void CCobFileHandler::RestoreMutableCode(const std::vector<std::string>& names, const std::vector<std::vector<int>>& codes)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const size_t count = std::min(names.size(), codes.size());

	if (names.size() != codes.size()) {
		LOG_L(L_WARNING, "[CobFileHandler::%s] mutable COB code state has mismatched name/code counts: %u/%u",
			__func__,
			static_cast<unsigned int>(names.size()),
			static_cast<unsigned int>(codes.size())
		);
	}

	for (size_t i = 0; i < count; ++i) {
		CCobFile* file = GetCobFile(names[i]);

		if (file == nullptr) {
			LOG_L(L_WARNING, "[CobFileHandler::%s] could not restore mutable COB code for missing script \"%s\"",
				__func__,
				names[i].c_str()
			);
			continue;
		}

		if (file->code.size() != codes[i].size()) {
			LOG_L(L_WARNING, "[CobFileHandler::%s] could not restore mutable COB code for script \"%s\" with mismatched code size: %u/%u",
				__func__,
				names[i].c_str(),
				static_cast<unsigned int>(file->code.size()),
				static_cast<unsigned int>(codes[i].size())
			);
			continue;
		}

		file->code = codes[i];
		LogReplayCheckpointDebugCobFile("restore", names[i], file, true);
	}
}
