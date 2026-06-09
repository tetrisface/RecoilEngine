#ifndef SYNC_UPDATED_PATHS_SYSTEM_UTILS_H_
#define SYNC_UPDATED_PATHS_SYSTEM_UTILS_H_

#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Path/QTPFS/Registry.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

namespace QTPFS {

	inline bool ReplayCheckpointDebugFinishPathFrame()
	{
		const int debugFrame = configHandler->GetInt("ReplayCheckpointDebugSignatureFrame");
		return (debugFrame >= 0 && gs != nullptr && gs->frameNum == debugFrame);
	}

	inline int ReplayCheckpointPathOwnerID(const IPath* path)
	{
		if (path == nullptr || path->GetOwner() == nullptr)
			return -1;

		return path->GetOwner()->id;
	}

	inline void FinishPathSearch
        ( PathManager* pm
		, PathSearch* search
        )
	{
        auto completePath = [pm, search](QTPFS::entity pathEntity, IPath* path){
			const int ownerBefore = ReplayCheckpointPathOwnerID(path);
			const SearchModeIPath* searchModePath = registry.try_get<SearchModeIPath>(pathEntity);
			const int ownerSearch = ReplayCheckpointPathOwnerID(searchModePath);

            // Transfer search update path, to the simulation-visible path.
            (*path) = std::move(registry.get<SearchModeIPath>(pathEntity));

			if (ReplayCheckpointDebugFinishPathFrame()) {
				LOG("[ReplayCheckpoint][qtpfs-finish] complete frame=%d path=%u searchPath=%u raw=%u found=%u ownerBefore=%d ownerSearch=%d ownerAfter=%d points=%u nodes=%u hash=%08x",
					gs->frameNum,
					static_cast<unsigned int>(entt::to_integral(pathEntity)),
					search != nullptr ? search->GetID() : 0u,
					(search != nullptr && search->rawPathCheck) ? 1u : 0u,
					(search != nullptr && search->PathWasFound()) ? 1u : 0u,
					ownerBefore,
					ownerSearch,
					ReplayCheckpointPathOwnerID(path),
					path->NumPoints(),
					path->NumNodes(),
					path->CalculateHash()
				);
			}

            // inform the movement system that the path has been changed.
            if (registry.all_of<PathUpdatedCounterIncrease>(pathEntity)) {
                path->SetNumPathUpdates(path->GetNumPathUpdates() + 1);
                path->SetNextPointIndex(0);
                registry.remove<PathUpdatedCounterIncrease>(pathEntity);
            }
            registry.remove<PathIsTemp>(pathEntity);
            registry.remove<PathIsDirty>(pathEntity);
            registry.remove<PathSearchRef>(pathEntity);

            // If the node data wasn't recorded, then the path isn't shareable.
            if (!path->IsBoundingBoxOverriden() || path->GetNodeList().size() == 0) {
                pm->RemovePathFromShared(pathEntity);
                pm->RemovePathFromPartialShared(pathEntity);
            }
        };

		QTPFS::entity pathEntity = (QTPFS::entity)search->GetID();
		if (registry.valid(pathEntity)) {
			// Only owned paths should be actioned in this function.
			IPath* path = registry.try_get<IPath>(pathEntity);
			if (path != nullptr) {
				if (search->PathWasFound()) {
					completePath(pathEntity, path);
					// LOG("%s: %x - path found", __func__, entt::to_integral(pathEntity));
				} else {
					if (search->rawPathCheck) {
						registry.remove<PathSearchRef>(pathEntity);
						registry.remove<PathIsDirty>(pathEntity);

						// adding a new search doesn't break this loop because new paths do not
						// have the tag ProcessPath and so don't impact this group view.
						pm->RequeueSearch(path, false, true, search->tryPathRepair);
						// LOG("%s: %x - raw path check failed", __func__, entt::to_integral(pathEntity));
					} else if (search->pathRequestWaiting) {
						// nothing to do - it will be rerun next frame
						// LOG("%s: %x - waiting for partial root path", __func__, entt::to_integral(pathEntity));
						// continue;
						registry.remove<PathSearchRef>(pathEntity);
						pm->RequeueSearch(path, false, search->allowPartialSearch, false);
					} else if (search->rejectPartialSearch) {
						registry.remove<PathSearchRef>(pathEntity);
						pm->RequeueSearch(path, false, false, false);
					}
					else {
						// LOG("%s: %x - search failed", __func__, entt::to_integral(pathEntity));
						// Don't invalid the path, now, give the unit the chance to escape from
						// being stuck inside something.
						path->SetBoundingBox();
						completePath(pathEntity, path);
					}
				}
			}
		}
	}

}

#endif
