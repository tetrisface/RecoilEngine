/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RemoveDeadPathsSystem.h"

#include <cassert>
#include <limits>

#include "Map/ReadMap.h"

#include "Sim/Misc/GlobalSynced.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Path/QTPFS/Components/RemoveDeadPaths.h"
#include "Sim/Path/QTPFS/NodeLayer.h"
#include "Sim/Path/QTPFS/PathManager.h"
#include "Sim/Path/QTPFS/Registry.h"

#include "System/Ecs/EcsMain.h"
#include "System/Ecs/Utils/SystemGlobalUtils.h"
#include "System/Log/ILog.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"
#include "System/SpringMath.h"

#include "System/Misc/TracyDefs.h"

#include <algorithm>
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

    template<typename View>
    std::vector<QTPFS::entity> CollectSortedPathEntities(View&& view)
    {
        std::vector<QTPFS::entity> entities;

        for (const QTPFS::entity entity: view) {
            entities.push_back(entity);
        }

        std::sort(entities.begin(), entities.end(), LessPathEntity);
        return entities;
    }
}

void RemoveDeadPathsSystem::Init()
{
    RECOIL_DETAILED_TRACY_ZONE;

    auto& comp = systemGlobals.CreateSystemComponent<RemoveDeadPathsComponent>();
    comp.deleteAll = false;

    systemUtils.OnUpdate().connect<&RemoveDeadPathsSystem::Update>();
}

void RemoveDeadPathsSystem::Update()
{
    SCOPED_TIMER("ECS::RemoveDeadPathsSystem::Update");

    auto& comp = systemGlobals.GetSystemComponent<RemoveDeadPathsComponent>();
    if (gs->frameNum % comp.refreshRate != comp.refreshOffset && !comp.deleteAll) return;

    auto* pm = dynamic_cast<PathManager*>(pathManager);

    auto view = registry.view<PathDelayedDelete>();
    const std::vector<QTPFS::entity> pathEntities = CollectSortedPathEntities(view);
    for (const QTPFS::entity pathEntity: pathEntities) {
        if (!registry.valid(pathEntity) || !registry.all_of<PathDelayedDelete>(pathEntity))
            continue;

        auto deleteOnFrame = view.get<PathDelayedDelete>(pathEntity).value;
        if (comp.deleteAll || gs->frameNum > deleteOnFrame) {
            pm->DeletePathEntity(pathEntity);
        }
    }
}

void RemoveDeadPathsSystem::Shutdown() {
    RECOIL_DETAILED_TRACY_ZONE;

    auto& comp = systemGlobals.GetSystemComponent<RemoveDeadPathsComponent>();
    comp.deleteAll = true;

    Update();

    systemUtils.OnUpdate().disconnect<&RemoveDeadPathsSystem::Update>();
}
