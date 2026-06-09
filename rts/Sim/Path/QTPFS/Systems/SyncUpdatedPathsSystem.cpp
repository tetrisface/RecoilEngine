/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// #undef NDEBUG

#include "SyncUpdatedPathsSystem.h"

#include "Sim/Path/IPathManager.h"
#include "Sim/Path/QTPFS/Components/Path.h"
#include "Sim/Path/QTPFS/Components/SyncUpdatedPaths.h"
#include "Sim/Path/QTPFS/PathManager.h"
#include "Sim/Path/QTPFS/Registry.h"
#include "Sim/Path/QTPFS/Utils/DestroyEntityUtils.h"
#include "Sim/Path/QTPFS/Utils/SyncUpdatedPathsSystemUtils.h"
#include "Sim/Objects/SolidObject.h"

#include "System/Ecs/EcsMain.h"
#include "System/Ecs/Utils/SystemGlobalUtils.h"
#include "System/TimeProfiler.h"
#include "System/Log/ILog.h"

#include "System/Misc/TracyDefs.h"

#include <algorithm>
#include <limits>
#include <vector>

using namespace SystemGlobals;
using namespace QTPFS;

namespace {
    unsigned int GetPathOwnerID(const IPath* path)
    {
        if (path == nullptr || path->GetOwner() == nullptr)
            return std::numeric_limits<unsigned int>::max();

        return static_cast<unsigned int>(path->GetOwner()->id);
    }

    int CompareFloat3(const float3& lhs, const float3& rhs)
    {
        if (lhs.x != rhs.x)
            return (lhs.x < rhs.x) ? -1 : 1;
        if (lhs.y != rhs.y)
            return (lhs.y < rhs.y) ? -1 : 1;
        if (lhs.z != rhs.z)
            return (lhs.z < rhs.z) ? -1 : 1;

        return 0;
    }

    bool LessPathEntity(const QTPFS::entity lhs, const QTPFS::entity rhs)
    {
        if (lhs == rhs)
            return false;

        const IPath* lhsPath = registry.valid(lhs) ? registry.try_get<IPath>(lhs) : nullptr;
        const IPath* rhsPath = registry.valid(rhs) ? registry.try_get<IPath>(rhs) : nullptr;

        if (lhsPath == nullptr || rhsPath == nullptr) {
            if (lhsPath != rhsPath)
                return lhsPath != nullptr;

            return entt::to_integral(lhs) < entt::to_integral(rhs);
        }

        const unsigned int lhsOwnerID = GetPathOwnerID(lhsPath);
        const unsigned int rhsOwnerID = GetPathOwnerID(rhsPath);

        if (lhsOwnerID != rhsOwnerID)
            return lhsOwnerID < rhsOwnerID;
        if (lhsPath->GetPathType() != rhsPath->GetPathType())
            return lhsPath->GetPathType() < rhsPath->GetPathType();

        if (const int sourceCmp = CompareFloat3(lhsPath->GetSourcePoint(), rhsPath->GetSourcePoint()); sourceCmp != 0)
            return sourceCmp < 0;
        if (const int targetCmp = CompareFloat3(lhsPath->GetTargetPoint(), rhsPath->GetTargetPoint()); targetCmp != 0)
            return targetCmp < 0;
        if (const int goalCmp = CompareFloat3(lhsPath->GetGoalPosition(), rhsPath->GetGoalPosition()); goalCmp != 0)
            return goalCmp < 0;

        return entt::to_integral(lhs) < entt::to_integral(rhs);
    }

    bool LessPathSearchEntity(const QTPFS::entity lhs, const QTPFS::entity rhs)
    {
        if (lhs == rhs)
            return false;

        const PathSearch* lhsSearch = registry.valid(lhs) ? registry.try_get<PathSearch>(lhs) : nullptr;
        const PathSearch* rhsSearch = registry.valid(rhs) ? registry.try_get<PathSearch>(rhs) : nullptr;
        const QTPFS::entity lhsPathEntity = (lhsSearch != nullptr) ? QTPFS::entity(lhsSearch->GetID()) : entt::null;
        const QTPFS::entity rhsPathEntity = (rhsSearch != nullptr) ? QTPFS::entity(rhsSearch->GetID()) : entt::null;

        if (LessPathEntity(lhsPathEntity, rhsPathEntity))
            return true;
        if (LessPathEntity(rhsPathEntity, lhsPathEntity))
            return false;

        return entt::to_integral(lhs) < entt::to_integral(rhs);
    }

    template<typename View>
    std::vector<QTPFS::entity> CollectSortedPathSearchEntities(View&& view)
    {
        std::vector<QTPFS::entity> entities;

        for (const QTPFS::entity entity: view) {
            entities.push_back(entity);
        }

        std::sort(entities.begin(), entities.end(), LessPathSearchEntity);
        return entities;
    }
}

void SyncUpdatedPathsSystem::Init()
{
    RECOIL_DETAILED_TRACY_ZONE;
    auto& comp = systemGlobals.CreateSystemComponent<SyncUpdatedPathsComponent>();
    systemUtils.OnUpdate().connect<&SyncUpdatedPathsSystem::Update>();
}

void SyncUpdatedPathsSystem::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;
    SCOPED_TIMER("ECS::SyncUpdatedPathsSystem::Update");

    auto* pm = dynamic_cast<PathManager*>(pathManager);
    auto& comp = systemGlobals.GetSystemComponent<SyncUpdatedPathsComponent>();

    // Ensure any outstanding pathing tasks are completed before synchronisation.
	if (comp.backgroundTask){
		wait_for_mt_background(comp.backgroundTask);
		comp.backgroundTask.reset();
	}

    auto pathSearchView = registry.group<PathSearch, ProcessPath>();
    const std::vector<QTPFS::entity> pathSearchEntities = CollectSortedPathSearchEntities(pathSearchView);

	for (const QTPFS::entity pathSearchEntity: pathSearchEntities) {
		if (!registry.valid(pathSearchEntity) || !registry.all_of<PathSearch>(pathSearchEntity))
			continue;

		PathSearch* search = &registry.get<PathSearch>(pathSearchEntity);
		// assert(search->rawPathCheck == false); // raw path checks should have been processed already
		FinishPathSearch(pm, search);

		// LOG("%s: delete search %x", __func__, entt::to_integral(pathSearchEntity));
		if (registry.valid(pathSearchEntity))
			DestroyPathSearchEntity(pathSearchEntity);
	}
}

void SyncUpdatedPathsSystem::Shutdown() {
    RECOIL_DETAILED_TRACY_ZONE;
    systemUtils.OnUpdate().disconnect<&SyncUpdatedPathsSystem::Update>();

	// drain the task group if it still exists, to ensure all tasks have completed before we shutdown.
	auto& comp = systemGlobals.GetSystemComponent<SyncUpdatedPathsComponent>();
	if (comp.backgroundTask){
		wait_for_mt_background(comp.backgroundTask);
		comp.backgroundTask.reset();
	}
}
