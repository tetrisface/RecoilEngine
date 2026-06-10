/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "CobEngine.h"

#include "CobDeferredCallin.h"
#include "CobThread.h"
#include "CobFile.h"

#include <cstdint>
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Units/Unit.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Misc/TracyDefs.h"
#include "Lua/LuaUI.h"

CR_BIND(CCobEngine, )

CR_REG_METADATA(CCobEngine, (
	CR_MEMBER(threadInstances),
	CR_MEMBER(tickAddedThreads),
	CR_MEMBER(tickRemovedThreads),
	CR_MEMBER(runningThreadIDs),
	CR_MEMBER(waitingThreadIDs),
	CR_MEMBER(sleepingThreadIDs),

	CR_IGNORED(curThread),
	CR_IGNORED(deferredCallins),

	CR_MEMBER(currentTime),
	CR_MEMBER(threadCounter)
))

CR_BIND(CCobEngine::SleepingThread, )
CR_REG_METADATA(CCobEngine::SleepingThread, (
	CR_MEMBER(id),
	CR_MEMBER(wt)
))

static const char* const numCobThreadsPlot = "CobThreads";
static bool ReplayCheckpointDebugCobFrame()
{
	return (gs != nullptr && configHandler != nullptr && gs->frameNum == configHandler->GetInt("ReplayCheckpointDebugSignatureFrame"));
}

static int ReplayCheckpointCobOwnerID(const CCobThread* thread)
{
	if (thread == nullptr || thread->cobInst == nullptr || thread->cobInst->GetUnit() == nullptr)
		return -1;

	return thread->cobInst->GetUnit()->id;
}

static bool ReplayCheckpointShouldLogCobThread(const CCobThread* thread)
{
	const int debugUnitID = configHandler->GetInt("ReplayCheckpointDebugCobUnitID");
	return (debugUnitID == -1 || ReplayCheckpointCobOwnerID(thread) == debugUnitID);
}

static void LogReplayCheckpointCobThread(const char* phase, const CCobThread* thread, int currentTime)
{
	if (!ReplayCheckpointDebugCobFrame() || thread == nullptr || !ReplayCheckpointShouldLogCobThread(thread))
		return;

	LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d id=%d owner=%d state=%d wake=%d pc=%d wait=%d|%d sig=%d call=%u|%08x data=%u|%08x lua=%08x",
		phase,
		gs->frameNum,
		currentTime,
		thread->GetID(),
		ReplayCheckpointCobOwnerID(thread),
		static_cast<int>(thread->GetState()),
		thread->GetWakeTime(),
		thread->GetProgramCounter(),
		thread->GetWaitAxis(),
		thread->GetWaitPiece(),
		thread->GetSignalMask(),
		static_cast<unsigned int>(thread->GetCallStackSize()),
		thread->GetCallStackChecksum(),
		static_cast<unsigned int>(thread->GetDataStackSize()),
		thread->GetDataStackChecksum(),
		thread->GetLuaArgsChecksum()
	);
}

static void LogReplayCheckpointCobQueueIDs(const char* phase, const char* queueName, const std::vector<int>& threadIDs, CCobEngine* engine)
{
	if (!ReplayCheckpointDebugCobFrame())
		return;

	LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d queue=%s count=%u",
		phase,
		gs->frameNum,
		engine->GetCurrTime(),
		queueName,
		static_cast<unsigned int>(threadIDs.size())
	);

	for (size_t index = 0; index < threadIDs.size(); ++index) {
		const int threadID = threadIDs[index];
		const CCobThread* thread = engine->GetThread(threadID);
		if (!ReplayCheckpointShouldLogCobThread(thread))
			continue;

		LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d queue=%s index=%u id=%d owner=%d state=%d wake=%d pc=%d wait=%d|%d",
			phase,
			gs->frameNum,
			engine->GetCurrTime(),
			queueName,
			static_cast<unsigned int>(index),
			threadID,
			ReplayCheckpointCobOwnerID(thread),
			(thread != nullptr) ? static_cast<int>(thread->GetState()) : -1,
			(thread != nullptr) ? thread->GetWakeTime() : -1,
			(thread != nullptr) ? thread->GetProgramCounter() : -1,
			(thread != nullptr) ? thread->GetWaitAxis() : -1,
			(thread != nullptr) ? thread->GetWaitPiece() : -1
		);
	}
}

static void LogReplayCheckpointCobTickAdded(const char* phase, const std::vector<CCobThread>& threads, int currentTime)
{
	if (!ReplayCheckpointDebugCobFrame())
		return;

	LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d queue=tickAdded count=%u",
		phase,
		gs->frameNum,
		currentTime,
		static_cast<unsigned int>(threads.size())
	);

	for (size_t index = 0; index < threads.size(); ++index) {
		const CCobThread& thread = threads[index];
		if (!ReplayCheckpointShouldLogCobThread(&thread))
			continue;

		LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d queue=tickAdded index=%u id=%d owner=%d state=%d wake=%d pc=%d wait=%d|%d",
			phase,
			gs->frameNum,
			currentTime,
			static_cast<unsigned int>(index),
			thread.GetID(),
			ReplayCheckpointCobOwnerID(&thread),
			static_cast<int>(thread.GetState()),
			thread.GetWakeTime(),
			thread.GetProgramCounter(),
			thread.GetWaitAxis(),
			thread.GetWaitPiece()
		);
	}
}

static void LogReplayCheckpointCobQueues(const char* phase, CCobEngine* engine)
{
	if (!ReplayCheckpointDebugCobFrame())
		return;

	LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d threads=%u running=%u waiting=%u sleeping=%u tickAdded=%u tickRemoved=%u counter=%d",
		phase,
		gs->frameNum,
		engine->GetCurrTime(),
		static_cast<unsigned int>(engine->GetThreadInstances().size()),
		static_cast<unsigned int>(engine->GetRunningThreadIDs().size()),
		static_cast<unsigned int>(engine->GetWaitingThreadIDs().size()),
		static_cast<unsigned int>(engine->GetSleepingThreadIDs().size()),
		static_cast<unsigned int>(engine->GetTickAddedThreads().size()),
		static_cast<unsigned int>(engine->GetTickRemovedThreads().size()),
		engine->GetThreadCounter()
	);

	LogReplayCheckpointCobQueueIDs(phase, "running", engine->GetRunningThreadIDs(), engine);
	LogReplayCheckpointCobQueueIDs(phase, "waiting", engine->GetWaitingThreadIDs(), engine);
	LogReplayCheckpointCobTickAdded(phase, engine->GetTickAddedThreads(), engine->GetCurrTime());
	LogReplayCheckpointCobQueueIDs(phase, "tickRemoved", engine->GetTickRemovedThreads(), engine);

	auto sleepingThreads = engine->GetSleepingThreadIDs();
	unsigned int index = 0;
	while (!sleepingThreads.empty()) {
		const CCobEngine::SleepingThread sleeper = sleepingThreads.top();
		sleepingThreads.pop();

		const CCobThread* thread = engine->GetThread(sleeper.id);
		if (!ReplayCheckpointShouldLogCobThread(thread)) {
			++index;
			continue;
		}

		LOG("[ReplayCheckpoint][cob] %s frame=%d time=%d queue=sleeping index=%u id=%d storedWake=%d owner=%d state=%d wake=%d pc=%d wait=%d|%d",
			phase,
			gs->frameNum,
			engine->GetCurrTime(),
			index++,
			sleeper.id,
			sleeper.wt,
			ReplayCheckpointCobOwnerID(thread),
			(thread != nullptr) ? static_cast<int>(thread->GetState()) : -1,
			(thread != nullptr) ? thread->GetWakeTime() : -1,
			(thread != nullptr) ? thread->GetProgramCounter() : -1,
			(thread != nullptr) ? thread->GetWaitAxis() : -1,
			(thread != nullptr) ? thread->GetWaitPiece() : -1
		);
	}
}

int CCobEngine::AddThread(CCobThread&& thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (thread.GetID() == -1)
		thread.SetID(GenThreadID());

	LogReplayCheckpointCobThread("add-before", &thread, currentTime);

	CCobInstance* o = thread.cobInst;
	CCobThread& t = threadInstances[thread.GetID()];

	// move thread into registry, hand its ID to owner
	t = std::move(thread);
	o->AddThreadID(t.GetID());
	LogReplayCheckpointCobThread("add-after", &t, currentTime);

	TracyPlot(numCobThreadsPlot, static_cast<int64_t>(threadInstances.size()));

	return (t.GetID());
}

bool CCobEngine::RemoveThread(int threadID) {
	RECOIL_DETAILED_TRACY_ZONE;
	const auto it = threadInstances.find(threadID);

	if (it != threadInstances.end()) {
		threadInstances.erase(it);
		TracyPlot(numCobThreadsPlot, static_cast<int64_t>(threadInstances.size()));
		return true;
	}

	return false;
}

void CCobEngine::ProcessQueuedThreads() {
	ZoneScoped;

	// Remove threads killed during Tick by other thread (SIGNAL), we do it
	// here as nothing is actively referencing any thread's memory here.
	for (int threadID: tickRemovedThreads) {
		RemoveThread(threadID);
	}
	tickRemovedThreads.clear();

	// move new threads spawned by START into threadInstances;
	// their ID's will already have been scheduled into either
	// waitingThreadIDs or sleepingThreadIDs
	for (CCobThread& t: tickAddedThreads) {
		AddThread(std::move(t));
	}

	tickAddedThreads.clear();
}

// a thread wants to continue running at a later time, and adds itself to the scheduler
void CCobEngine::ScheduleThread(const CCobThread* thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	LogReplayCheckpointCobThread("schedule", thread, currentTime);
	switch (thread->GetState()) {
		case CCobThread::Run: {
			waitingThreadIDs.push_back(thread->GetID());
		} break;
		case CCobThread::Sleep: {
			sleepingThreadIDs.push(SleepingThread{thread->GetID(), thread->GetWakeTime()});
		} break;
		default: {
			LOG_L(L_ERROR, "[COBEngine::%s] unknown state %d for thread %d", __func__, thread->GetState(), thread->GetID());
		} break;
	}
}

void CCobEngine::SanityCheckThreads(const CCobInstance* owner)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (false) {
		// no threads belonging to owner should be left
		for (const auto& p: threadInstances) {
			assert(p.second.cobInst != owner);
		}
		for (const CCobThread& t: tickAddedThreads) {
			assert(t.cobInst != owner);
		}
	}
}


void CCobEngine::TickThread(CCobThread* thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	LogReplayCheckpointCobThread("tick-thread-begin", thread, currentTime);
	// for error messages originating in CUnitScript
	curThread = thread;

	// NB: threadID is still in <runningThreadIDs> here, TickRunningThreads clears it
	if (thread != nullptr && !thread->Tick()) {
		LogReplayCheckpointCobThread("tick-thread-dead", thread, currentTime);
		RemoveThread(thread->GetID());
	} else {
		LogReplayCheckpointCobThread("tick-thread-end", thread, currentTime);
	}

	curThread = nullptr;
}

void CCobEngine::WakeSleepingThreads()
{
	ZoneScoped;
	// check on the sleeping threads, remove any whose owner died
	while (!sleepingThreadIDs.empty()) {
		const SleepingThread sleeper = sleepingThreadIDs.top();
		CCobThread* zzzThread = GetThread(sleeper.id);
		if (ReplayCheckpointDebugCobFrame() && (zzzThread == nullptr || ReplayCheckpointShouldLogCobThread(zzzThread))) {
			LOG("[ReplayCheckpoint][cob] wake-top frame=%d time=%d id=%d storedWake=%d owner=%d state=%d wake=%d",
				gs->frameNum,
				currentTime,
				sleeper.id,
				sleeper.wt,
				ReplayCheckpointCobOwnerID(zzzThread),
				(zzzThread != nullptr) ? static_cast<int>(zzzThread->GetState()) : -1,
				(zzzThread != nullptr) ? zzzThread->GetWakeTime() : -1
			);
		}

		if (zzzThread == nullptr) {
			sleepingThreadIDs.pop();
			continue;
		}

		// not yet time to execute this thread or any subsequent sleepers
		if (zzzThread->GetWakeTime() >= currentTime)
			break;

		// remove executing thread from the queue
		sleepingThreadIDs.pop();

		// wake up the thread and tick it (if not dead)
		// this can quite possibly re-add the thread to <sleepingThreadIDs>
		// again, but any thread is guaranteed to sleep for at least 1 tick
		switch (zzzThread->GetState()) {
			case CCobThread::Sleep: {
				zzzThread->SetState(CCobThread::Run);
				TickThread(zzzThread);
			} break;
			case CCobThread::Dead: {
				RemoveThread(zzzThread->GetID());
			} break;
			default: {
				LOG_L(L_ERROR, "[COBEngine::%s] unknown state %d for thread %d", __func__, zzzThread->GetState(), zzzThread->GetID());
			} break;
		}
	}
}

void CCobEngine::TickRunningThreads()
{
	ZoneScoped;
	// advance all currently running threads
	for (const int threadID: runningThreadIDs) {
		TickThread(GetThread(threadID));
	}

	// a thread can never go from running->running, so clear the list
	// note: if preemption was to be added, this would no longer hold
	// however, TA scripts can not run preemptively anyway since there
	// aren't any synchronization methods available
	runningThreadIDs.clear();

	// prepare threads that will run next frame
	std::swap(runningThreadIDs, waitingThreadIDs);
}

void CCobEngine::Tick(int deltaTime)
{
	ZoneScoped;
	LogReplayCheckpointCobQueues("tick-start", this);
	currentTime += deltaTime;
	LogReplayCheckpointCobQueues("after-time", this);

	TickRunningThreads();
	LogReplayCheckpointCobQueues("after-running", this);
	ProcessQueuedThreads();

	WakeSleepingThreads();
	LogReplayCheckpointCobQueues("after-wake", this);
	ProcessQueuedThreads();
	LogReplayCheckpointCobQueues("tick-end", this);
}


void CCobEngine::ShowScriptError(const std::string& msg)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (curThread != nullptr) {
		curThread->ShowError(msg.c_str());
		return;
	}

	LOG_L(L_ERROR, "[COBEngine::%s] \"%s\" outside script execution", __func__, msg.c_str());
}


void CCobEngine::AddDeferredCallin(CCobDeferredCallin&& deferredCallin)
{
	deferredCallins[deferredCallin.funcHash].push_back(deferredCallin);
}


void CCobEngine::RunDeferredCallins()
{
	std::vector<int> funcHashes;
	funcHashes.reserve(deferredCallins.size());
	for(auto& it: deferredCallins)
		funcHashes.push_back(it.first);

	for(auto funcHash: funcHashes) {
		auto it = deferredCallins.find(funcHash); // 'it' has to necessarily be present at this point

		auto callins = std::move(it->second);
		deferredCallins.erase(it);

		const LuaHashString cmdStr = LuaHashString(callins[0].funcName.c_str());
		luaRules->unsyncedLuaHandle.Cob2LuaBatch(cmdStr, callins);
		if (luaUI)
			luaUI->Cob2LuaBatch(cmdStr, callins);
	}
}
