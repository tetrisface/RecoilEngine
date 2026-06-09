/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#ifdef SYNCCHECK

#include "SyncChecker.h"

// This cannot be included in the header file (SyncChecker.h) because include conflicts will occur.
#if !defined(UNIT_TEST)
	#include "Sim/Misc/GlobalSynced.h"
	#include "System/Config/ConfigHandler.h"
	#include "System/Config/ConfigVariable.h"
	#include "System/Log/ILog.h"
#endif
#include "System/Threading/ThreadPool.h"
#include "System/HashSpec.h"

#include <cstdint>
#include <limits>

#if !defined(UNIT_TEST)
CONFIG(int, ReplayCheckpointDebugSyncTraceFrame).defaultValue(-1).description("Log replay checkpoint sync-check write trace at this frame; -1 disables it.");
CONFIG(int, ReplayCheckpointDebugSyncTraceStart).defaultValue(0).minimumValue(0).description("First replay checkpoint sync-check trace entry to log for the selected frame.");
CONFIG(int, ReplayCheckpointDebugSyncTraceLimit).defaultValue(0).minimumValue(0).description("Maximum replay checkpoint sync-check trace entries to log for the selected frame.");
#endif


unsigned CSyncChecker::g_checksum;
int CSyncChecker::inSyncedCode;

#if !defined(UNIT_TEST)
static int replayCheckpointSyncTraceCachedFrame = std::numeric_limits<int>::min();
static int replayCheckpointSyncTraceFrame = -1;
static int replayCheckpointSyncTraceStart = 0;
static int replayCheckpointSyncTraceLimit = 0;
static unsigned int replayCheckpointSyncTraceIndex = 0;

static void LogReplayCheckpointSyncTrace(
	const char* kind,
	const char* msg,
	unsigned int size,
	unsigned int valueHash,
	unsigned int valueRaw,
	unsigned int checksumBefore,
	unsigned int checksumAfter
) {
	if (gs == nullptr || configHandler == nullptr)
		return;

	const int frame = gs->frameNum;
	if (frame != replayCheckpointSyncTraceCachedFrame) {
		replayCheckpointSyncTraceCachedFrame = frame;
		replayCheckpointSyncTraceFrame = configHandler->GetInt("ReplayCheckpointDebugSyncTraceFrame");
		replayCheckpointSyncTraceStart = configHandler->GetInt("ReplayCheckpointDebugSyncTraceStart");
		replayCheckpointSyncTraceLimit = configHandler->GetInt("ReplayCheckpointDebugSyncTraceLimit");
		replayCheckpointSyncTraceIndex = 0;
	}

	if (frame != replayCheckpointSyncTraceFrame || replayCheckpointSyncTraceLimit <= 0)
		return;

	const unsigned int index = replayCheckpointSyncTraceIndex++;
	const unsigned int start = static_cast<unsigned int>(replayCheckpointSyncTraceStart);
	if (index < start)
		return;
	if ((index - start) >= static_cast<unsigned int>(replayCheckpointSyncTraceLimit))
		return;

	LOG("[ReplayCheckpoint][sync-trace] frame=%d index=%u kind=%s op=%s size=%u valueHash=%08x valueRaw=%08x valueS16=%d before=%08x after=%08x",
		frame,
		index,
		kind,
		(msg != nullptr) ? msg : "",
		size,
		valueHash,
		valueRaw,
		static_cast<int>(static_cast<int16_t>(valueRaw & 0xffffu)),
		checksumBefore,
		checksumAfter
	);
}
#else
static void LogReplayCheckpointSyncTrace(
	const char*,
	const char*,
	unsigned int,
	unsigned int,
	unsigned int,
	unsigned int,
	unsigned int
) {
}
#endif

void CSyncChecker::NewFrame()
{
	g_checksum = 0xfade1eaf;
#ifdef SYNC_HISTORY
	LogHistory();
#endif // SYNC_HISTORY
}

void CSyncChecker::debugSyncCheckThreading()
{
	assert(ThreadPool::GetThreadNum() == 0);
}

void CSyncChecker::Sync(uint32_t val)
{
	SyncTagged(val, nullptr);
}

void CSyncChecker::SyncTagged(uint32_t val, const char* msg)
{
#ifdef DEBUG_SYNC_MT_CHECK
	// Sync calls should not be occurring in multi-threaded sections
	debugSyncCheckThreading();
#endif
	const unsigned int checksumBefore = g_checksum;
	g_checksum = spring::hash_combine(val, g_checksum);
	LogReplayCheckpointSyncTrace("uint32", msg, sizeof(val), val, val, checksumBefore, g_checksum);
	//LOG("[Sync::Checker] chksum=%u\n", g_checksum);

#ifdef SYNC_HISTORY
	LogHistory();
#endif // SYNC_HISTORY
}

void CSyncChecker::Sync(const void* p, unsigned size)
{
	SyncTagged(p, size, nullptr);
}

void CSyncChecker::SyncTagged(const void* p, unsigned size, const char* msg)
{
#ifdef DEBUG_SYNC_MT_CHECK
	// Sync calls should not be occurring in multi-threaded sections
	debugSyncCheckThreading();
#endif
	const unsigned int checksumBefore = g_checksum;
	const unsigned int valueHash = spring::LiteHash(p, size, 0u);
	unsigned int valueRaw = 0;
	const auto* bytes = static_cast<const std::uint8_t*>(p);
	for (unsigned int i = 0; i < size && i < sizeof(valueRaw); ++i) {
		valueRaw |= static_cast<unsigned int>(bytes[i]) << (i * 8u);
	}

	// most common cases first, make it easy for compiler to optimize for it
	// simple xor is not enough to detect multiple zeroes, e.g.
	g_checksum = spring::LiteHash(p, size, g_checksum);
	LogReplayCheckpointSyncTrace("bytes", msg, size, valueHash, valueRaw, checksumBefore, g_checksum);
	//LOG("[Sync::Checker] chksum=%u\n", g_checksum);

#ifdef SYNC_HISTORY
	LogHistory();
#endif // SYNC_HISTORY
}

#ifdef SYNC_HISTORY

unsigned CSyncChecker::nextHistoryIndex = 0;
unsigned CSyncChecker::nextFrameIndex = 0;
std::array<unsigned, MAX_SYNC_HISTORY> CSyncChecker::logs;
std::array<unsigned, MAX_SYNC_HISTORY_FRAMES> CSyncChecker::logFrames;

void CSyncChecker::NewGameFrame()
{
	logFrames[nextFrameIndex++] = nextHistoryIndex;
	if (nextFrameIndex == MAX_SYNC_HISTORY_FRAMES)
		nextFrameIndex = 0;
}

void CSyncChecker::LogHistory()
{
	logs[nextHistoryIndex++] = g_checksum;
	if (nextHistoryIndex == MAX_SYNC_HISTORY)
		nextHistoryIndex = 0;
}

std::tuple<unsigned, unsigned, unsigned*> CSyncChecker::GetFrameHistory(unsigned rewindFrames)
{
	int endFrameIndex = nextFrameIndex - rewindFrames;
	int startFrameIndex = endFrameIndex - 1;

	if (endFrameIndex < 0)
		endFrameIndex = MAX_SYNC_HISTORY_FRAMES + endFrameIndex;
	if (startFrameIndex < 0)
		startFrameIndex = MAX_SYNC_HISTORY_FRAMES + startFrameIndex;

	return std::make_tuple(logFrames[startFrameIndex], logFrames[endFrameIndex], logs.data());
}

#endif // SYNC_HISTORY

#endif // SYNCCHECK
