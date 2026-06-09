/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef QTPFS_PATHMANAGER_HDR
#define QTPFS_PATHMANAGER_HDR

#include <vector>

#include "Sim/Misc/ModInfo.h"
#include "Sim/Path/IPathManager.h"
#include "NodeLayer.h"
#include "PathCache.h"
#include "PathSearch.h"
#include "System/UnorderedMap.hpp"

struct MoveDef;
struct SRectangle;
class CSolidObject;


namespace QTPFS {
	struct QTNode;
	class PathManager: public IPathManager {
	public:
		// must not be larger than the smallest evenly divisible size of maps.
		static constexpr unsigned int DAMAGE_MAP_BLOCK_SIZE = 16;

		struct MapChangeTrack {
			std::vector<bool> damageMap;
			std::deque<int> damageQueue;
		};
		struct NodeLayersChangeTrack {
			std::vector<MapChangeTrack> mapChangeTrackers;
			int width = 0;
			int height = 0;
			int cellSize = 0;
		};

		PathManager();
		~PathManager();

		static void InitStatic();

		std::int32_t GetPathFinderType() const override { return QTPFS_TYPE; }
		std::uint32_t GetPathCheckSum() const override { return pfsCheckSum; }

		std::int64_t Finalize() override;
		std::int64_t PostFinalizeRefresh() override;

		bool PathUpdated(unsigned int pathID) override;
		void ClearPathUpdated(unsigned int pathID) override;

		bool AllowShortestPath() override { return true; }

		void TerrainChange(unsigned int x1, unsigned int z1,  unsigned int x2, unsigned int z2, unsigned int type) override;
		void Update() override;
		void UpdatePath(const CSolidObject* owner, unsigned int pathID) override;
		void DeletePath(unsigned int pathID, bool force = false) override;
		void ResetLivePathsForLoad() override;
		void RebuildReplayCheckpointNodeLayersForLoad() override;
		bool RestoreReplayCheckpointPathsForLoad() override;
		void RestoreReplayCheckpointPathAllocator() override;
		void SerializeReplayCheckpointState(creg::ISerializer* s) override;
		void LogReplayCheckpointStateSignature(const char* label) const override;
		void DeletePathEntity(QTPFS::entity pathEntity);

		unsigned int RequestPath(
			CSolidObject* object,
			const MoveDef* moveDef,
			float3 sourcePos,
			float3 targetPos,
			float radius,
		bool synced,
		bool immediateResult = false
	) override;

	unsigned int RequestPathWithID(
		CSolidObject* object,
		const MoveDef* moveDef,
		float3 sourcePos,
		float3 targetPos,
		float radius,
		bool synced,
		unsigned int preferredPathID,
		bool immediateResult = false
	) override;

		float3 NextWayPoint(
			const CSolidObject*, // owner
			unsigned int pathID,
			unsigned int, // numRetries
			float3 point,
			float radius,
			bool synced
		) override;

		bool CurrentWaypointIsUnreachable(unsigned int pathID) override;
		bool NextWayPointIsUnreachable(unsigned int pathID) override;

		void GetPathWayPoints(
			unsigned int pathID,
			std::vector<float3>& points,
			std::vector<int>& starts
		) const override;

		int2 GetNumQueuedUpdates() const override;


		const NodeLayer& GetNodeLayer(unsigned int pathType) const { return nodeLayers[pathType]; }
		const NodeLayersChangeTrack& GetMapDamageTrack() const { return nodeLayersMapDamageTrack; };

		const spring::unordered_map<unsigned int, PathSearchTrace::Execution*>& GetPathTraces() const { return pathTraces; }

		void RemovePathFromShared(QTPFS::entity entity);
		void RemovePathFromPartialShared(QTPFS::entity entity);

	private:
		struct ReplayCheckpointPathNodeState {
			uint32_t nodeId = -1U;
			uint32_t nodeNumber = -1U;
			float2 netPoint;
			int pathPointIndex = -1;
			int xmin = 0;
			int zmin = 0;
			int xmax = 0;
			int zmax = 0;
			bool badNode = false;
		};

		struct ReplayCheckpointPathState {
			QTPFS::entity entity = entt::null;
			bool hasOwner = false;
			uint32_t ownerID = 0;
			uint32_t pathID = 0;
			uint32_t nextPointIndex = 0;
			uint32_t repathTriggerIndex = 0;
			uint32_t numPathUpdates = 0;
			uint32_t firstCleanNodeID = 0;
			PathHashType hash = BAD_HASH;
			PathHashType virtualHash = BAD_HASH;
			float radius = 0.0f;
			float3 boundingBoxMins;
			float3 boundingBoxMaxs;
			float3 goalPosition;
			spring_time searchTime;
			int pathType = 0;
			bool synced = true;
			bool fullPath = true;
			bool partialPath = false;
			bool rawPath = false;
			bool boundingBoxOverride = false;
			bool unsyncedPath = false;
			bool externalPath = false;
			bool dirty = false;
			bool temp = false;
			bool toBeUpdated = false;
			bool updatedCounterIncrease = false;
			bool requeueSearch = false;
			bool requeueSearchValue = false;
			bool searchModePath = false;
			bool delayedDelete = false;
			int delayedDeleteFrame = 0;
			bool sharedPathChain = false;
			bool sharedPathHead = false;
			QTPFS::entity sharedPrev = entt::null;
			QTPFS::entity sharedNext = entt::null;
			bool partialSharedPathChain = false;
			bool partialSharedPathHead = false;
			QTPFS::entity partialSharedPrev = entt::null;
			QTPFS::entity partialSharedNext = entt::null;
			std::vector<float3> points;
			std::vector<ReplayCheckpointPathNodeState> nodes;
		};

		void CaptureReplayCheckpointPathStates();
		void CaptureReplayCheckpointEmptyEntities();
		void SerializeReplayCheckpointPathState(creg::ISerializer* s, ReplayCheckpointPathState& state);
		void RestoreReplayCheckpointSharedPathCaches();
		void RestoreReplayCheckpointPathComponentOrder();
		void PruneReplayCheckpointExtraEmptyEntities();

		void MapChanged(int x1, int z1, int x2, int z2);

		void ThreadUpdate();
		void Load();

		std::uint64_t GetMemFootPrint() const;

		typedef void (PathManager::*MemberFunc)(
			unsigned int threadNum,
			unsigned int numThreads,
			const SRectangle& rect
		);
		typedef spring::unordered_map<unsigned int, unsigned int> PathTypeMap;
		typedef spring::unordered_map<unsigned int, unsigned int>::iterator PathTypeMapIt;
		typedef spring::unordered_map<unsigned int, PathSearchTrace::Execution*> PathTraceMap;
		typedef spring::unordered_map<unsigned int, PathSearchTrace::Execution*>::iterator PathTraceMapIt;
		typedef spring::unordered_map<PathHashType, QTPFS::entity> SharedPathMap;
		typedef spring::unordered_map<PathHashType, QTPFS::entity>::iterator SharedPathMapIt;
		typedef spring::unordered_map<PathHashType, QTPFS::entity> PartialSharedPathMap;
		typedef spring::unordered_map<PathHashType, QTPFS::entity>::iterator PartialSharedPathMapIt;

		typedef std::vector<PathSearch*> PathSearchVect;
		typedef std::vector<PathSearch*>::iterator PathSearchVectIt;

		void InitNodeLayersThreaded(const SRectangle& rect, bool reportLoadScreen = true);
		void InitNodeLayer(unsigned int layerNum, const SRectangle& r);
		void InitRootSize(const SRectangle& r);
		void UpdateNodeLayer(unsigned int layerNum, const SRectangle& r, int currentThread);

		bool InitializeSearch(QTPFS::entity searchEntity);
		void RemovePathSearch(QTPFS::entity pathEntity);

		void ReadyQueuedSearches();
		void ProcessPathSearch(QTPFS::entity pathSearchEntity, bool shouldBeRaw);
		void ExecuteQueuedSearches();
		void NormalizePathAllocatorFreeList();
		void QueueDeadPathSearches();

		unsigned int QueueSearch(
			const CSolidObject* object,
			const MoveDef* moveDef,
			const float3& sourcePoint,
			const float3& targetPoint,
			const float radius,
			const bool synced,
			const bool externalRequest,
			const bool allowRawSearch,
			const unsigned int preferredPathID = 0
		);

	public:
		unsigned int RequeueSearch(
			IPath* oldPath,
			const bool allowRawSearch,
			const bool allowPartialSearch,
			const bool allowRepair
		);

	private:
		bool ExecuteSearch(
			PathSearch* search,
			NodeLayer& nodeLayer,
			unsigned int pathType,
			bool immediateSearch
		);

		unsigned int ExecuteImmediateSearch(unsigned int pathId);

		bool IsFinalized() const { return isFinalized; }

	public:
		std::vector<NodeLayer> nodeLayers;

	private:
		PathCache pathCache;

		// per thread data
		std::vector<SearchThreadData> searchThreadData;
		std::vector<UpdateThreadData> updateThreadData;
		std::vector<unsigned char> nodeLayerUpdatePriorityOrder;

		PathTraceMap pathTraces;
		SharedPathMap sharedPaths;
		PartialSharedPathMap partialSharedPaths;

		// std::vector<unsigned int> numCurrExecutedSearches;
		// std::vector<unsigned int> numPrevExecutedSearches;

		NodeLayersChangeTrack nodeLayersMapDamageTrack;

		int deadPathsToUpdatePerFrame = 1;
		int recalcDeadPathUpdateRateOnFrame = 0;
		int rootSize = 0;

		static unsigned int LAYERS_PER_UPDATE;
		static unsigned int MAX_TEAM_SEARCHES;

		unsigned int searchStateOffset;
		unsigned int numPathRequests;

		std::int32_t refreshDirtyPathRateFrame = QTPFS_LAST_FRAME;
		std::int32_t updateDirtyPathRate = 0;
		std::int32_t updateDirtyPathRemainder = 0;

		std::uint32_t pfsCheckSum;

		QTPFS::entity systemEntity = entt::null;
		std::vector<QTPFS::entity> replayCheckpointRegistryEntities;
		std::vector<QTPFS::entity> replayCheckpointEmptyEntities;
		std::vector<ReplayCheckpointPathState> replayCheckpointPathStates;
		QTPFS::entity replayCheckpointRegistryReleased = entt::null;
		bool replayCheckpointRegistryLoaded = false;
		bool replayCheckpointPathStatesLoaded = false;

		bool isFinalized = false;

		static constexpr size_t INITIAL_PATH_RESERVE = 256;
	};
}

#endif
