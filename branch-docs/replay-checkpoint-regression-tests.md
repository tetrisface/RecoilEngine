# Replay Checkpoint Regression Tests

This document tracks regression cases for replay checkpoint restore. The goal is
to turn each desync we find into a focused test target, even if the current
engine shape requires smoke tests before smaller unit tests are practical.

Upstream design context and source links are summarized in
[`replay-checkpoint-upstream-notes.md`](replay-checkpoint-upstream-notes.md).

## Restart Cleanup 2026-06-22

The active branch now targets Timeline Core: checkpoint-owned forward/backward
jumps, pause preservation, bounded catch-up, checkpoint tick marks, and
SYNCCHECK/sync-hash equivalence. BAR transition overlays, screenshot automation,
GUI-shader/range cleanup windows, and broader unsynced visual cleanup
experiments were archived on
`replay-archive/2026-06-22-unsynced-gui-experiments` at `d3445bd20d20`. Older
regression entries that mention those systems remain useful historical evidence,
but they are not active acceptance gates.

## Acceptance Invariant

For a deterministic SYNCCHECK replay with checkpoint bundle:

1. Restore to checkpoint frame `R`.
2. Resume playback from `R`.
3. Every sync check from `R` onward matches the original replay path.
4. No `DESYNC WARNING`, sync-hash mismatch, or hidden fast-forward shortcut is
   accepted as success.

The scripted smoke path and the BAR replay timeline UI path both need to satisfy
this invariant.

## Current Smoke Fixture

Use the Windows scripts until the engine has smaller injectable seams:

```powershell
.\scripts\replay-timeline-record-synctest-windows.ps1 `
  -SpringExe .\RecoilEngine\build-windows\install\spring.exe `
  -DurationFrames 900 `
  -RequireSyncHash

.\scripts\replay-checkpoint-smoke-windows.ps1 `
  -RunDir .\.cache\replay-timeline-synctest\<run> `
  -SpringExe .\RecoilEngine\build-windows\install\spring.exe `
  -SaveFrame 90 `
  -LoadFrame 180 `
  -TargetFrame 90 `
  -ResumeFrame 181 `
  -LuaRulesSelfTest `
  -RequireSynctestMarkers `
  -RequireSyncChecksumRestore
```

When the smoke resumes far enough to emit the restored replay digest, add
`-RequirePostRestoreSyncHash`; it restarts the Lua synctest checksum capture
after restore, requires a contiguous restored frame span through the synctest
end frame, and compares every emitted frame against the recorded replay artifact.

This is an integration test, not a unit test, but it is currently the strongest
evidence because it exercises CREG load, Lua state, demo replay, frame
accounting, command handling, and sync checks together.

The Windows replay scripts mute both music and master sound while running:
`snd_volmusic = 0` and `snd_volmaster = 0`.

## Regression Cases

### COB Scheduler Wait State

Bug shape:

- `CCobEngine::waitingThreadIDs` was ignored by CREG.
- A frame-90 restore produced different waiting thread ids from the original
  path immediately after load.
- The next resumed frame diverged through different COB scheduling behavior.

Test target:

- Construct or fixture-load a `CCobEngine` with non-empty
  `waitingThreadIDs`.
- Save and load through CREG.
- Assert `waitingThreadIDs`, sleeping thread ids, and script thread ownership
  are preserved.

Future seam:

- A small CREG round-trip harness for individual synced components.
- No game window, map load, or demo reader should be required for this case.

Current status:

- Fixed in source by serializing `waitingThreadIDs`.
- Verified improvement: frame-90 original and restored dumps now preserve
  waiting and sleeping COB thread ids.
- Not sufficient for replay rewind correctness; later resume still desyncs.

### Ground Move Path Rebuild

Bug shape:

- `CGroundMoveType::PostLoad()` discarded the saved `pathID` and requested a
  replacement path.
- The old path request did not also rebuild `currWayPoint`, `nextWayPoint`,
  `earlyCurrWayPoint`, or `earlyNextWayPoint`.
- The first resumed frame diverged in unit movement state even when unit counts,
  feature counts, projectile counts, and RNG state matched.

Test target:

- Restore a ground unit with active movement, non-zero `pathID`, and a pending
  move command.
- After `PostLoad()`, assert the replacement path id and waypoint fields are
  internally consistent with the path manager result.
- Resume one sim frame and assert position, heading, progress state, and command
  queue match an uninterrupted replay path.

Future seam:

- Inject a deterministic fake path manager into `CGroundMoveType` so
  `PostLoad()` can be tested without a full map/pathfinder.
- Keep a higher-level replay fixture because real QTPFS behavior may still
  expose bugs a fake path manager cannot.

Current status:

- Still unresolved.
- A naive attempt to call `GetNewPath()` from `PostLoad()` was rejected because
  `GetNewPath()` has a near-goal early return that changes prior load behavior.
- A second attempt that preserved the old unconditional `RequestPath()` call but
  also refreshed waypoints changed the restored replay path and still desynced:
  restored frame 120 became `sync=15f5d983 units=375 features=10` while the
  original replay path was `sync=1c71c713 units=367 features=8`.
- A synchronous QTPFS rebuild from `CGroundMoveType::PostLoad()` was rejected:
  calling `RequestPath(..., immediateResult=true)` during CREG object postload
  crashed before checkpoint restore reached demo playback reset.
- Moving `pathManager->ResetLivePathsForLoad()` before CREG `LoadGame()` was
  also rejected: it avoided the crash but still desynced, with restored frame
  120 becoming `sync=d5dfa19e units=377 features=10`.
- A preferred-QTPFS-path-ID rebuild inside `CGroundMoveType::PostLoad()` was
  rejected. It ran before `CGame::LoadReplayCheckpoint()` called
  `pathManager->ResetLivePathsForLoad()`, so the rebuilt paths were then
  deleted. The run logged requested IDs being remapped, followed by:
  `reset QTPFS live paths for load: 333 paths, 116 searches`.
- Moving the preferred-ID rebuild after `ResetLivePathsForLoad()` was in the
  correct phase, but still not sufficient: entt did not recreate the same raw
  path IDs after the restored entities had been destroyed. Fresh fixture
  `.cache/replay-timeline-synctest/run_20260605_003246` restored f91 as
  `sync=fae37455` instead of original `sync=1947f604`; f120 desynced
  `fec3fd92 -> 87397fec`, and f180 desynced `6a761137 -> af2d1334`.
- Preserving CREG-loaded QTPFS `IPath` entities instead of deleting and
  re-requesting them avoided path ID remap warnings, but is still incomplete.
  It logged
  `reset QTPFS live paths for load: preserved 243 paths, removed 0 searches, cleared 0 search refs`,
  but fresh fixture `.cache/replay-timeline-synctest/run_20260605_004845`
  restored f91 as `sync=2fb3cda4` instead of original `sync=22ee2357`; f120
  desynced `7b1cced4 -> 1714b156`, and f180 desynced
  `9a4b946b -> 54e959ce`.
- The next test target should compare CREG-restored QTPFS path internals and
  `CGroundMoveType` waypoint/progress state after load, before replay resume.
  Path IDs alone are not enough.
- Added a gated QTPFS diagnostic for that target:
  `ReplayCheckpointDebugSignatureFrame=<frame>` now logs `[ReplayCheckpoint][qtpfs-sig]`
  aggregates and `[ReplayCheckpoint][qtpfs-path]` path details through the
  `IPathManager` diagnostic seam.
- Smoke fixture `.cache/replay-timeline-synctest/run_20260605_143557` with
  `SaveFrame=90 LoadFrame=180 TargetFrame=90 ResumeFrame=181 DebugSignatureFrame=91`
  showed that QTPFS is already divergent at the first restored frame. Original
  frame 91 `simframe-begin` had `paths=227 pathHash=86d28eac`; restored frame 91
  had `paths=114 pathHash=1adddf21` while the general game signature still
  matched (`sync=4cc0a5f3 units=354 syncedProjectiles=219`).
- The same run showed original frame 91 `after-path-update` as
  `paths=124 pathHash=74b9606f`, while restored frame 91 had
  `paths=118 pathHash=da586620`. The run resumed to frame 181 but failed the
  invariant with `DESYNC WARNING` at replay sync frames 120 and 180.
- A guard that skips `RestoreReplayCheckpointPathAllocator()` while rebuilt
  QTPFS searches are still live removed the prior frame-103 crash in
  `QTPFS::PathManager::ExecuteQueuedSearches`, but it is not a correctness fix:
  it only avoids rewiring the allocator under live searches and exposes the
  remaining path-state desync.
- Next test seam: restore or round-trip QTPFS path/search/cache state directly,
  or make `CGroundMoveType` reconstruction prove exact path population,
  `PathSearchRef`, temp/requeue flags, shared-path cache state, and waypoint
  progress before any replay frame resumes.

## 2026-06-09 QTPFS Allocator Restore Attempt

Bug shape:

- The allocator/order restoration attempt now restores QTPFS registry structure and empty placeholders after explicit path replay restore, but `PathManager::Update()` still diverges by frame-91 unit/path stage.
- The same smoke run records two frame-91 signature streams: one before checkpoint restore and one immediately after restore.

Attempted fix and evidence:

- Run: `.cache/replay-timeline-synctest/run_20260609_004724`
- Bundle: `run_20260609_004724/write_episode_0/demos/rcp_533a9a6c.replay-checkpoints`
- Smoke command:
  `scripts/replay-checkpoint-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260609_004724 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -SaveFrame 90 -LoadFrame 180 -TargetFrame 90 -ResumeFrame 181 -DebugSignatureFrame 91 -LuaRulesSelfTest -RequireSynctestMarkers -TimeoutSeconds 300`
- Playback logs show:
  - `[ReplayCheckpoint] reset QTPFS registry for load: removed 262 live entities (217 paths, 0 searches)`
  - `[ReplayCheckpoint] pruned 43 extra empty QTPFS registry entities after load`
  - Simframe-begin frame 91 signatures match across both streams:
    - game: `sync=42dffe2b`, `units=356`, `features=4`, `syncedProjectiles=187`
    - qtpfs: `live=312 paths=267 nonPath=45 empty=44 ... searchRefs=0 searchMode=267 delayedDelete=136 pathHash=a1664fe2 searchHash=8d12f3ab`
  - The split now appears at frame-91 unit/path phase:
    - `after-unit-update` game sync changes from `89352776` to `c8e73ffa`
    - `after-unit-update` qtpfs-path hash changes from `c3db5321` to `7757eb9b`
    - `after-path-update` qtpfs-path hash changes from `3856be5a` to `dab2513d`
    - by `after-projectile-update`, search hash differs (`56b9474d` vs `4b4021a1`)
- The run still fails sync equivalence:
  - frame 120: expected `71887be3`, got `7378617f`
  - frame 180: expected `5eb093ad`, got `c126dd15`

Current seam:

- Trace queued searches and allocator reuse at the unit-update boundary, then compare deleted path IDs, search queue ordering, shared-chain updates, and qtpfs path hashes in a deterministic frame-91 fixture.

### Replay Restore Frame Accounting

Bug shape:

- Checkpoint restore must leave the viewer at the restored frame without using
  server-side replay `skip f<frame>` as a fake backward jump.
- Pause state can be mutated by save-load internals and must be restored by the
  replay checkpoint path.

Test target:

- Request load from frame `L` to target frame `R`.
- Assert `gs->frameNum`, server frame accounting, demo reader frame, and Lua
  observed frame all report `R`.
- Assert the viewer remains paused after restore when it was paused before
  restore.
- Assert no catch-up simulation frames are processed as the mechanism for a
  backward jump.

Future seam:

- Extract a checkpoint restore context that owns frame accounting, pause
  preservation, and demo-reader positioning.
- Unit-test that context with fake game server/demo reader objects.

### Sync Hash Resume Equivalence

Bug shape:

- A restore can appear visually successful and still desync after resume.
- Frame-local state dumps may match selected subsystems while omitted state
  still diverges.

Test target:

- Record original sync signatures for every sync-check frame in a fixture.
- Restore from each checkpoint interval boundary.
- Resume past at least the next two sync checks.
- Assert original and restored sync hashes match exactly.

Future seam:

- Keep this as an integration regression test. It is intentionally broad and
  should remain the final gate for any replay checkpoint change.

### Checkpoint Bundle Integrity

Bug shape:

- A replay timeline may select the wrong checkpoint or load an incompatible
  checkpoint bundle.

Test target:

- Verify every bundle entry has frame, file, engine version, map, game/mod, and
  hash metadata.
- Assert nearest-checkpoint selection returns the latest checkpoint at or before
  the requested target frame.
- Reject checkpoints with incompatible engine/map/game metadata.

Future seam:

- Unit-test this in `recoil-replay-rs` and in the C++ resolver separately.
- Keep the current `replayctl verify-bundle` path as the command-line guard.

## Performance And Storage Tests

These tests belong after correctness is proven:

- Measure checkpoint save wall time relative to frame processing time.
- Assert keyframe saving adds less than 10 percent of frame processing time in
  the selected benchmark fixture.
- Assert checkpoint storage adds less than 50 percent over replay size, or no
  more than 2 MB, for the selected benchmark fixture.
- Assert the default interval is no worse than one keyframe per 5 minutes of
  game time.

Future storage work may parse `.ssf` internals and compare alternate encodings,
but no storage optimization should be treated as successful until restore/resume
sync equivalence is already green.

## Suggested Test Layers

1. Component CREG round-trip tests for individual state owners.
2. Restore-context unit tests with fake demo reader, fake server, and fake pause
   state.
3. Pathing/movement post-load tests with deterministic fake path manager.
4. Scripted SYNCCHECK replay smoke tests for end-to-end determinism.
5. BAR replay timeline UI smoke tests for the user-facing command path.

The smaller tests should catch known mistakes quickly. The SYNCCHECK replay test
remains the authority for whether rewind is actually safe.

## 2026-06-09 QTPFS Exact Path Snapshot Evidence

Bug shape:

- Rebuilding QTPFS paths from move types was not enough: restored path IDs could
  exist while path internals, owner pointers, shared path chains, search scratch
  components, delayed-delete markers, and allocator placeholders still diverged.
- Using only active entities for `entt::registry::assign` caused restore
  hangs/crashes. The raw entity table is needed for EnTT restore, but extra empty
  placeholders then had to be pruned to match the original live allocator state.

Attempted fixes and evidence:

- Added QTPFS checkpoint snapshots for path points, nodes, owner presence,
  `SearchModeIPath`, `PathDelayedDelete`, `SharedPathChain`,
  `PartialSharedPathChain`, and original empty placeholder entities.
- Fixed owner id `0` by storing `hasOwner` separately from `ownerID`.
- Fresh fixture `.cache/replay-timeline-synctest/run_20260609_000331`, bundle
  `rcp_1c4ae7fe.replay-checkpoints`, verified checkpoint frames 90/180/270.
- Smoke command:
  `scripts/replay-checkpoint-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260609_000331 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -SaveFrame 90 -LoadFrame 180 -TargetFrame 90 -ResumeFrame 181 -DebugSignatureFrame 91 -LuaRulesSelfTest -RequireSynctestMarkers -TimeoutSeconds 300`
- Restore pruned 43 extra empty QTPFS entities and restored 237 paths. At frame
  91 `simframe-begin`, original and restored now match on counts:
  `live=282 paths=237 nonPath=45 empty=44 searches=0 temp=24 requeue=237
  searchMode=237 delayedDelete=127 sharedChains=23 partialChains=30`, and all
  237 `[qtpfs-path] simframe-begin` entries match by entity and value.
- The run still desyncs: sync warnings at frames 120 and 180. The first narrowed
  QTPFS split is after `PathManager::Update()` on frame 91: original has
  `paths=126 dirty=0 delayedDelete=18 sharedChains=20 partialChains=26`, while
  restored has `paths=125 dirty=4 delayedDelete=17 sharedChains=18
  partialChains=23`.
- After-path-update path-set delta: original-only entities
  `131,181,1048884,3145907,4194591,5243158,6291550`; restored-only entities
  `46,94,1048657,4194430,6291516,6291558`. Four shared/partial paths remain
  dirty only after restore: entities `114`, `344`, `139`, and `1048697`.

Future seam:

- Compare or restore QTPFS search execution inputs and shared-chain head/order
  decisions through `PathManager::Update()`, not just the state visible before
  the update. A focused test should start from matching path snapshots and assert
  that one path update produces the same deleted paths, dirty flags, shared-chain
  removals, and search hashes.

### QTPFS Queue/Process Diagnostics Instrumentation

Attempted fix:

- Added frame-gated QTPFS logs in `RecoilEngine/rts/Sim/Path/QTPFS/PathManager.cpp`
  for `ReplayCheckpointDebugSignatureFrame`:
  - `qtpfs-queue` stream in `QueueSearch` (`queue-search`) and `RequeueSearch`
    (`requeue-search`)
  - `qtpfs-queue` snapshots in `ExecuteQueuedSearches` (`before-ready`,
    `after-ready`, `after-raw-clean`, `before-background`)
- This aims to isolate queue/scratch state transitions at the frame-91 unit-update
  boundary where divergence begins.

Current status:

- Executed on later fixture `.cache/replay-timeline-synctest/run_20260609_110541`.
  The queue/search logs showed path registry/request/chain state matching while
  the actual search result for path `5242982` diverged at frame 115. That moved
  the root cause from queue ordering to QTPFS node-layer graph state.

### QTPFS Node-Layer Graph Restore

Bug shape:

- QTPFS path snapshots could restore path entities, owners, chains, and queued
  request inputs exactly, but the node-layer graph used by `PathSearch` still
  reflected the later load-frame world.
- Frame 115 path `5242982` had matching source, target, owner, path type,
  `srcNode=7353`, `tgtNode=4975`, shared-chain head, and search thread in both
  streams, yet search execution diverged.
- Original frame 115 search: `fwdSearched=36 bwdSearched=35 fwdConnected=1
  bwdConnected=0 fwdTgtNode=6747 bwdTgtNode=6750`, final path
  `points=14 nodes=13 hash=64892c16`.
- Restored-before-fix frame 115 search: `fwdSearched=35 bwdSearched=35
  fwdConnected=0 bwdConnected=1 fwdTgtNode=6750 bwdTgtNode=6754`, final path
  `points=13 nodes=12 hash=51e80fbe`.
- The narrowed node evidence was QTPFS layer 11 node `4975`: original
  `layerHash=12e0885d cost=2.007874 neighbours=6 neighbourHash=bf0ac5be`;
  restored-before-fix `layerHash=8a7d2239 cost=2.0193019 neighbours=8
  neighbourHash=4cf3874b`.

Fix:

- Added the path-manager IoC hook
  `IPathManager::RebuildReplayCheckpointNodeLayersForLoad()`.
- QTPFS implements the hook by reinitializing node layers, node-layer map damage
  trackers, `PathSpeedModInfoSystem`, and `pfsCheckSum` from the restored map
  and blocking state.
- `CGame::LoadReplayCheckpoint()` now calls `readMap->UpdateHeightBounds()` and
  the hook after CREG restore and `ResetLivePathsForLoad()`, but before
  `RestoreReplayCheckpointPathsForLoad()`. This keeps QTPFS ownership inside the
  path manager and avoids CGame reaching into node-layer internals.
- `NodeLayer::Init()` now clears its existing pools, caches, counters, and root
  metadata before rebuilding so replay restore can reuse layer objects without
  tripping global QTPFS node registration lifetime.

Verified evidence:

- Build: Docker Windows SYNCCHECK build exited `0`.
- Fixture: `.cache/replay-timeline-synctest/run_20260609_110541`
- Bundle: `write_episode_0/demos/rcp_4c3b3d19.replay-checkpoints`
- Frame 115 probe:
  `.cache/replay-checkpoint-smoke/debug_signature_frame115_rebuilt_layers_*`
  passed to resume frame 117. Restored frame 115 now matches original node
  `4975` (`layerHash=12e0885d`, `neighbours=6`) and path `5242982` finalizes as
  `points=14 nodes=13 hash=64892c16`.
- Frame 120 probe:
  `.cache/replay-checkpoint-smoke/debug_signature_frame120_rebuilt_layers_*`
  passed to resume frame 122. Both streams have path `5242982` at simframe-begin
  with `points=14 nodes=13 pointHash=64892c16`.
- Frame 139 probe:
  `.cache/replay-checkpoint-smoke/debug_signature_frame139_rebuilt_layers_*`
  passed to resume frame 141. Both simframe-begin signatures match:
  `sync=acad9b00`, QTPFS `pathHash=a08f47c3`, `searchHash=8d12f3ab`.
- Full strict smoke:
  `.cache/replay-checkpoint-smoke/strict_resume181_after_qtpfs_rebuild_console.txt`
  passed with `SaveFrame=90 LoadFrame=121 TargetFrame=90 ResumeFrame=181`.
  The run restored to frame 90 paused and resumed to frame 181 with no
  `DESYNC WARNING` or sync-hash mismatch markers.
- LuaUI checkpoint smoke:
  `.cache/replay-checkpoint-smoke/strict_luaui_resume181_after_qtpfs_rebuild_console.txt`
  passed through `gui_replaybuttons.lua` with the same `SaveFrame=90
  LoadFrame=121 TargetFrame=90 ResumeFrame=181` window. The infolog shows the
  widget-issued load at frame 121, the engine restore to checkpoint frame 90,
  QTPFS node-layer rebuild, sync-check checksum restore
  `29b70229 -> 29b70229`, restored pause state, and resume to frame 181. No
  `DESYNC WARNING`, sync-hash mismatch, keyframe difference, or Lua traceback
  marker was found.
- BAR replay timeline forward smoke:
  `.cache/replay-timeline-smoke/run_20260609_110541_forward181_fullsync_console.txt`
  passed with `StartFrame=30 TargetFrame=181 QuitFrame=421`,
  `RequireSynctestMarkers`, and `RequireSyncHash`. The BAR replay widget reached
  frame 181, continued to the full 420-frame synctest end, and reproduced the
  recorded digest `xPXHk2p0w3oXfN+miaWxTg==` for frames `0..419`. A post-quit
  `Ecostats` DrawScreen warning appeared after sync hash emission and
  `QuitAction`; it did not affect replay checkpoint restore/resume evidence.

Future seam:

- Add a direct node-layer restore invariant after map/blocking restore: compare
  per-move-def layer checksums and selected node signatures before any path
  entities are restored or searches execute.
- Keep the end-to-end SYNCCHECK restore/resume smoke as the authority because
  matching node layers alone does not prove replay determinism.

### Later Checkpoint QTPFS Path-Set Mismatch

Bug shape:

- The node-layer rebuild fixed the checkpoint-90 restore/resume window, but a
  broader restore to checkpoint frame 180 from the same fixture still desyncs.
- Strict LuaRules smoke:
  `.cache/replay-checkpoint-smoke/strict_luarules_target180_resume300_after_qtpfs_rebuild_console.txt`
  used `SaveFrame=180 LoadFrame=241 TargetFrame=180 ResumeFrame=300`.
- The run restored frame 180 and resumed, then hit `DESYNC WARNING` at frame
  240: demo checksum `ca11ddd3`, restored checksum `8c1087a9`.

Observed evidence:

- A short debug run with `DebugSignatureFrame=181` passed to resume frame 182:
  `.cache/replay-checkpoint-smoke/debug_signature_frame181_target180_resume182_console.txt`.
- General `[ReplayCheckpoint][sig]` state matched original/restored at every
  frame-181 phase from `simframe-begin` through `simframe-end`, including units,
  features, projectiles, RNG, and sync checksum.
- QTPFS was already different at frame-181 `simframe-begin`, before that frame's
  simulation work:
  - original: `live=288 size=304 paths=243 delayedDelete=133 sharedChains=31
    partialChains=37 pathHash=0c02fc5c searchHash=8d12f3ab`
  - restored: `live=287 size=303 paths=242 delayedDelete=132 sharedChains=30
    partialChains=36 pathHash=45958dea searchHash=8d12f3ab`
- The mismatch is path entity population/ownership, not the node-layer graph:
  the diff has 101 original-only path entities, 100 restored-only path entities,
  and 10 shared entity IDs with different owner/path payload state.
- Representative changed shared entry: entity `200` is an ownerless delayed
  shared/partial path in the original (`owner=-1 type=38 hash=d006c495`) but a
  live owner path in restored state (`owner=2132 type=17 hash=07a4b313`).

Current suspicion:

- Later checkpoint bundles may be saving/restoring QTPFS path snapshots at a
  different lifecycle boundary than the replay comparison expects, or the path
  snapshot restore is losing delayed-delete/shared-chain ownership identity when
  the path set has churned longer.
- Next diagnostic should compare `DebugSignatureFrame=180` around the save/load
  boundary and then inspect `ReplayCheckpointHandler::UpdateRecordFrame()` plus
  QTPFS snapshot capture/restore timing.

### Later Checkpoint Restore: Owner, Feature Queue, And Smooth Mesh

This section supersedes the QTPFS-only suspicion above for the frame-180 restore
window. The later checkpoint failure needed three narrower fixes before the
cache-mutated smoke appeared to pass; the clean archive-consistent recheck below
shows restore/resume still has an open drift.

Bug shapes:

- QTPFS delayed shared/partial paths were restored with `owner=nullptr` even
  when the snapshot had serialized an owner. Preserving the serialized owner
  removed the path-owner mismatch as the leading explanation for the frame-240
  desync.
- `CFeatureHandler::updateFeatures` could keep stale idle feature entries after
  CREG load. Frame-181 evidence showed restored feature `24272` remained queued
  while the original path did not.
- `CReadMap::PostLoad()` calls full-map `mapDamage->RecalcArea()`, which calls
  `smoothGround.MapChanged()`. For replay checkpoints this was wrong because
  `SmoothHeightMesh` already serialized its mesh and pending update queues.
  Restore repopulated a full-map smooth damage queue, so frame 201 processed a
  different smooth-mesh work item.

Observed smooth-mesh evidence:

- Before the smooth guard, frame 180 original `simframe-end` had
  `smooth-sig hash=8fd15c2f queues=6/0/0/0`; restored `after-post-load` had
  `hash=4dee51c5 queues=192/0/0/0`.
- At frame 201, the visible smooth height sample matched before
  `UpdateSmoothMesh()`, but original processed a vertical blur item while
  restored processed damage/maxima work. Unit `21999` (`CStrafeAirMoveType`)
  then read `groundHeight=311.504364` on the original path and
  `311.504242` on the restored path, diverging immediately after
  `GeneralMoveSystem::Update()`.

Fixes:

- QTPFS restore now preserves serialized owners for delayed shared/partial path
  snapshots.
- `CFeatureHandler::RestoreUpdateQueueForLoad()` preserves valid serialized
  feature queue order, removes invalid/stale/duplicate entries, repairs
  `inUpdateQue`, and adds missing features that still need updates. The repair
  log for the frame-180 fixture was:
  `restored feature update queue after load: size=26 stale=1 invalid=0 duplicate=0 repairedFlags=0 added=0`.
- `SmoothHeightMesh` now has a replay-checkpoint load guard that suppresses
  `MapChanged()` notifications while `CCregLoadSaveHandler::LoadGame()` runs
  inside `CGame::LoadReplayCheckpoint()`. Read-map, LOS, feature, and pathing
  post-load refreshes can still run, but the smooth mesh keeps its serialized
  queue state.

Verified evidence:

- Build: Docker Windows SYNCCHECK target build exited `0`.
- Fixture: `.cache/replay-timeline-synctest/run_20260609_142625`
- Bundle:
  `write_episode_0/demos/rcp_f2ccdd39.replay-checkpoints`
- Reproducibility caveat: this fixture is historical evidence only. The cached
  BAR game directory was edited after recording to add the post-restore
  synctest restart, and later smoke logs for this fixture contain the engine
  warning `Archive Beyond-All-Reason.sdd ... differs from the host's copy`.
  These runs still document fix progression, but they are not final acceptance
  proof.
- Feature queue proof:
  `.cache/replay-checkpoint-smoke/debug_feature_queue_frame181_target180_resume182_after_feature_queue_restore_infolog.txt`
  matched frame-181 original/restored signatures through feature/script/end
  after the stale feature repair.
- Smooth restore-boundary proof:
  `.cache/replay-checkpoint-smoke/debug_smooth_mesh_frame180_target180_resume182_after_smooth_guard_infolog.txt`
  logged `suppressed 1 smooth mesh map-change notifications during checkpoint
  load`; restored `after-post-load` matched original `simframe-end` with
  `smooth-sig hash=8fd15c2f queues=6/0/0/0`.
- Frame-201 proof:
  `.cache/replay-checkpoint-smoke/debug_smooth_mesh_frame201_target180_resume202_after_smooth_guard_infolog.txt`
  matched smooth hashes in both streams:
  `simframe-begin hash=eb0d93da`, `after-smoothground-update hash=22107424`.
  Unit `21999` read the same strafe input in both streams
  (`groundHeight=311.504364`, `ctrl=<1,-0.286880136,1>`), and both streams had
  `after-general-move-system sync=ce5702cb unitHash=04076446`.
- Historical strict LuaRules smoke for the earlier checkpoint on that fixture:
  `.cache/replay-checkpoint-smoke/strict_luarules_target90_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with `SaveFrame=90 LoadFrame=121 TargetFrame=90 ResumeFrame=421`,
  `RequireSynctestMarkers`, `RequireSyncChecksumRestore`, and
  `RequirePostRestoreSyncHash`. The C++ SYNCCHECK restore marker was
  `restored sync-check checksum d1da9f70 for checkpoint frame 90`; the
  reconstructed digest artifact
  `.cache/replay-checkpoint-smoke/strict_luarules_target90_resume421_post_restore_synchash_after_smooth_guard_final_build_synchash.json`
  has digest `KkAnEdKkgjCguex7MIP6MQ==` for the contiguous 330-frame restored
  span `90..419`, and every emitted checksum matched the original replay
  artifact.
- Historical strict LuaRules smoke for the later checkpoint on that fixture:
  `.cache/replay-checkpoint-smoke/strict_luarules_target180_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with `SaveFrame=180 LoadFrame=241 TargetFrame=180 ResumeFrame=421`,
  `RequireSynctestMarkers`, `RequireSyncChecksumRestore`, and
  `RequirePostRestoreSyncHash`. The C++ SYNCCHECK restore marker was
  `restored sync-check checksum 3b37ccdd for checkpoint frame 180`; the
  reconstructed digest artifact
  `.cache/replay-checkpoint-smoke/strict_luarules_target180_resume421_post_restore_synchash_after_smooth_guard_final_build_synchash.json`
  has digest `f4P+6UsMt7vWK3LSw0l++Q==` for the contiguous 240-frame restored
  span `180..419`, and every emitted checksum matched the original replay
  artifact.
- Historical strict LuaUI replay-widget smoke for the earlier checkpoint on
  that fixture:
  `.cache/replay-checkpoint-smoke/strict_luaui_target90_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with `SaveFrame=90 LoadFrame=121 TargetFrame=90 ResumeFrame=421`,
  `RequireSynctestMarkers`, `RequireSyncChecksumRestore`, and
  `RequirePostRestoreSyncHash`. The C++ SYNCCHECK restore marker was
  `restored sync-check checksum d1da9f70 for checkpoint frame 90`; the
  reconstructed digest artifact
  `.cache/replay-checkpoint-smoke/strict_luaui_target90_resume421_post_restore_synchash_after_smooth_guard_final_build_synchash.json`
  has digest `KkAnEdKkgjCguex7MIP6MQ==` for the contiguous 330-frame restored
  span `90..419`, and every emitted checksum matched the original replay
  artifact.
- Historical strict LuaUI replay-widget smoke after the rebuild:
  `.cache/replay-checkpoint-smoke/strict_luaui_target180_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with `SaveFrame=180 LoadFrame=241 TargetFrame=180 ResumeFrame=421`,
  `RequireSynctestMarkers`, `RequireSyncChecksumRestore`, and
  `RequirePostRestoreSyncHash`. The log includes `gui_replaybuttons.lua`, the
  C++ SYNCCHECK restore marker `restored sync-check checksum 3b37ccdd for
  checkpoint frame 180`, and the opt-in Lua restart marker
  `sync-hash: restarted after replay checkpoint restore at frame 180`. The
  reconstructed post-restore digest artifact
  `.cache/replay-checkpoint-smoke/strict_luaui_target180_resume421_post_restore_synchash_after_smooth_guard_final_build_synchash.json`
  has digest `f4P+6UsMt7vWK3LSw0l++Q==` for the contiguous 240-frame restored
  span `180..419`, and every emitted checksum matched the original replay
  artifact. No `DESYNC WARNING`, sync error, sync-hash mismatch, or
  keyframe-difference marker appeared.

Clean archive-consistent recheck:

- Re-recorded fixture `.cache/replay-timeline-synctest/run_20260609_175435`
  after the local BAR harness change, so the replay and cached game archive both
  use the same `dbg_synctest.lua`. The recorded artifact has digest
  `Z2RQaqAOpChFwgKxO3ASnA==` for frames `0..419`, demo
  `write_episode_0/demos/2026-06-09_15-55-18-098_Comet Catcher Remake 1.8_2026.06.06-54-gd2e1651 keyframes-rewind-catchup.sdfz`,
  and bundle `write_episode_0/demos/rcp_20d5a147.replay-checkpoints`.
- BAR replay timeline forward smoke on the clean fixture passed:
  `.cache/replay-timeline-smoke/run_20260609_175435/write/infolog.txt`
  used `StartFrame=30 TargetFrame=181 QuitFrame=421` and reproduced digest
  `Z2RQaqAOpChFwgKxO3ASnA==` for frames `0..419` without archive mismatch.
- LuaRules restore to checkpoint `90` failed the acceptance invariant:
  `.cache/replay-checkpoint-smoke/clean_luarules_target90_resume421_desync_frame420_infolog.txt`
  restored the C++ SYNCCHECK boundary `653aa44d`, restarted post-restore
  checksum capture at frame `90`, then hit `DESYNC WARNING` at frame `420`
  (`demo=c065d92c`, restored `b60cc458`). Reconstructed artifact
  `.cache/replay-checkpoint-smoke/clean_luarules_target90_resume421_desync_frame420_synchash.json`
  has digest `38sMPRU8iyZqbVrwYRrtDg==` for `90..419`; per-frame comparison
  found 29 mismatches, first at frame `391` (`recorded=-1144226304`,
  `restored=876662720`).
- After moving the script's generic desync guard after post-restore hash
  reconstruction, rerun
  `.cache/replay-checkpoint-smoke/clean_luarules_target90_resume421_hash_guard_failure_infolog.txt`
  failed directly with `Post-restore sync hash mismatch at frame 391` and left
  `.cache/replay-checkpoint-smoke/clean_luarules_target90_resume421_hash_guard_failure_synchash.json`.
- LuaRules restore to checkpoint `180` also failed:
  `.cache/replay-checkpoint-smoke/clean_luarules_target180_resume421_desync_infolog.txt`
  restored the C++ SYNCCHECK boundary `50eefdc7`, then logged desync warnings at
  frames `240`, `300`, `360`, and `420` (`f240 demo=dedc92c8`, restored
  `c2d68818`). Reconstructed artifact
  `.cache/replay-checkpoint-smoke/clean_luarules_target180_resume421_desync_synchash.json`
  has digest `r7tmLxOACCQLllB5bM42aQ==` for `180..419`; per-frame comparison
  found 198 mismatches, first at frame `222` (`recorded=-1987333504`,
  `restored=-124341952`).

Current status:

- The clean fixture is now the authority for acceptance, and restore/resume is
  still open. The older fixture is useful history but is superseded for final
  proof because its archive checksum mismatch could hide reproducibility errors.
- The next diagnostic should probe the clean fixture around the first mismatch
  frames (`222` for checkpoint `180`, `391` for checkpoint `90`) with subsystem
  signatures and the narrowest available IoC seams: sync trace, smooth mesh,
  feature queue, QTPFS path/queue state, and move-system state at the exact
  resume-frame boundary.

Future seams:

- Add a `SmoothHeightMesh` CREG/load regression test that restores a serialized
  mesh with pending queues and proves read-map postload does not enqueue a
  full-map smooth update during replay checkpoint restore.
- Keep the smooth-mesh signature logger gated by
  `ReplayCheckpointDebugSignatureFrame`; it is useful for queue/hash comparison
  but should remain diagnostic-only.
- Move the opt-in post-restore Lua digest restart toward a smaller harness seam
  or engine-owned replay artifact. The current UI smoke now compares a fresh
  post-restore `sync-hash-json` artifact, but normal Lua reload still resets the
  unsynced `dbg_synctest` buffer unless the harness explicitly restarts it.

### LocalModel Bounding Volume PostLoad Restore

This section supersedes the clean target-180 desync as the current scripted
LuaRules checkpoint-180 evidence. A fresh fixture now passes strict sync
equivalence after preserving serialized `LocalModel` bounds during post-load.

Bug shape:

- Fresh fixture `.cache/replay-timeline-synctest/run_20260610_104413`, seed
  `1029845188`, recorded digest `D4tO0Ewvwpu43PsA3uk8vQ==`, bundle
  `write_episode_0/demos/rcp_8d86d30b.replay-checkpoints`.
- Before the fix, strict restore `SaveFrame=180 LoadFrame=301 TargetFrame=180
  ResumeFrame=421` restored the SYNCCHECK boundary but failed at frame `336`:
  restored checksum `-576125568`, recorded checksum `-1769042688`.
- Narrowing found the first meaningful gameplay split at frame `187`.
  Projectile `10008` and unit `11277` had matching projectile signatures,
  positions, script animation state, and visible collidable piece transforms.
  The original path directly hit unit `11277`; restored missed the unit and
  applied a weaker ground splash.
- The mismatched collision input was the aggregate unit local-model bounds.
  Original frame-187 bounds were `bvScales=<44.332207,28.784286,47.243370>`
  and `bvOffsets=<-0.583166,10.486351,-0.516272>`. Restored had
  `bvScales=<43.625885,25.943113,46.648232>` and
  `bvOffsets=<-0.184360,12.120708,-0.218700>`.
- Root cause: `CSolidObject::PostLoad()` called `LocalModel::SetModel(model,
  false)` to reattach model-piece pointers after CREG load, but that path also
  recalculated the aggregate `LocalModel::boundingVolume`. For replay checkpoint
  restore, CREG had already restored the exact serialized aggregate bounds and
  `needsBoundariesRecalc` flag, so post-load overwrote authoritative state with
  a recomputed value from the load-frame model state.

Fix:

- `LocalModel::SetModel(model, false)` now preserves the serialized
  `boundingVolume` and `needsBoundariesRecalc` values while still reattaching
  `S3DModelPiece` pointers and rebuilding piece child transforms.
- This keeps ownership local: CREG owns exact checkpoint state, `LocalModel`
  owns post-load pointer reattachment, and projectile collision code consumes
  the restored bounds without needing a replay-checkpoint special case.

Verified evidence:

- Docker Windows SYNCCHECK build exited `0` after the LocalModel fix and the
  focused diagnostic-gating cleanup.
- Focused frame-187 probe:
  `.cache/replay-checkpoint-smoke/debug_frame187_target180_after_unit_script_filter_infolog.txt`.
  Original/restored local-model hash both became `2a5929f9`, bounds matched
  exactly, and both streams logged projectile `10008` hitting piece `base`
  (`hitPieceModel=0`, `hitPieceScript=3`) with damage `270.138397`; unit
  `11277` health became `3528.035400` in both streams.
- Current strict LuaRules smoke:
  `.cache/replay-checkpoint-smoke/strict_target180_resume421_after_localmodel_bounds_unit_script_filter_infolog.txt`
  passed with `SaveFrame=180 LoadFrame=301 TargetFrame=180 ResumeFrame=421`,
  `RequireSynctestMarkers`, `RequireSyncChecksumRestore`, and
  `RequirePostRestoreSyncHash`. The run restored to frame `180` paused,
  resumed to frame `421`, and matched digest `VkJSDt8CijCeDsL6mNY/2Q==` for
  all `240` restored frames `180..419`.
- Diagnostic gating cleanup: `ReplayCheckpointDebugCobUnitID` now defaults to
  `-2` so a generic `ReplayCheckpointDebugSignatureFrame` run does not enable
  heavyweight COB/unit-script traces. Use `-1` only for explicit all-unit traces
  or a non-negative unit id for focused traces.
- Gate verification after the cleanup: the Windows SYNCCHECK compile-only build
  exited `0`, then
  `.cache/replay-checkpoint-smoke/debug_signature_frame271_target270_no_cob_default_after_diag_gate_infolog.txt`
  restored `SaveFrame=270` from request frame `361`, resumed to frame `272`,
  and emitted signature/QTPFS restore markers with `DebugSignatureFrame=271`.
  With default `ReplayCheckpointDebugCobUnitID=-2`, the log had `0` matches for
  `[ReplayCheckpoint][cob-op]`, `[cob-stack]`, `[cob-rng]`,
  `[unit-script-tick]`, or `[unit-script-explode]`.
- Owner-local regression added:
  `test/engine/Rendering/Models/testLocalModel.cpp` builds
  `test_LocalModel.exe` and exercises `LocalModel::SetModel(model, false)` with
  synthetic same-piece-count models. It asserts that post-load reattaches the
  `S3DModelPiece` pointer while preserving the serialized aggregate collision
  volume scales, offsets, bounding radius, and `needsBoundariesRecalc` flag.
  Verified with `test_LocalModel.exe --success`: 10 assertions in 1 test case.
  The full Windows SYNCCHECK compile-only build also exited `0` after adding
  this test target and the `UpdateList.cpp` include cleanup.

Future seam:

- Extend the `LocalModel` test into a CREG round trip once a small CREG harness
  can construct/load this owner directly. The current test locks down the
  post-load ownership boundary without booting a map or renderer.
- Add a smaller collision regression around the piece-tree early-out path:
  with identical projectile/unit/piece transforms, changing only the aggregate
  local-model bounds must be enough to reproduce the old hit-vs-splash split.

### Exact Checkpoint Save And QTPFS Carry-Over Searches

This section supersedes the target-270 QTPFS path-set mismatch observed after
the LocalModel fix. The latest seeded fixture restores later checkpoints and
passes strict scripted sync equivalence through the recorded end frame.

Bug shapes:

- Deferred replay checkpoint saves could drift by frame. A filename like
  `replaycheckpoint_000180.ssf` could contain a later `gs->frameNum` because
  `ReplayCheckpointHandler::QueueSaveCurrentFrame()` queued a generic
  `game->Save(...)` request and the save body ran after more sim frames.
- A restored checkpoint could match the original at the checkpoint boundary and
  first resumed frame, then diverge when `PathManager::Update()` finished a
  carry-over QTPFS search that was present in the original but absent after
  restore.
- In the target-270 failure fixture, two paths (`3145905` owner `13688` and
  `6291625` owner `23319`) were normal partial paths in the original frame-272
  state but remained temp two-point paths after restore. The missing state was
  not the live `IPath` payload alone; it was the active `PathSearchRef`,
  `ProcessPath`, and completed `SearchModeIPath` result flags that the next
  QTPFS update consumed.
- Checkpoint restore preserved empty QTPFS registry placeholders for allocator
  determinism. That is useful during restore, but the destructor reported those
  placeholders as still-active entities at shutdown, obscuring real cleanup
  leaks in smoke evidence.

Fixes:

- `ReplayCheckpointHandler::QueueSaveCurrentFrame()` now creates the checkpoint
  immediately with `ILoadSaveHandler::CreateSave(...)`, so frame-addressed
  artifacts are owned by the checkpoint recorder instead of the generic deferred
  save queue.
- QTPFS checkpoint state now includes a separate `SearchModeIPath` snapshot and
  replay checkpoint search states for `PathSearch`, `UnsyncedPathSearch`, and
  `ExternallyManagedPathSearch`. Restore recreates the search entity, path ref,
  `ProcessPath`, and full/partial-result flags before pruning the restored
  registry.
- `FeatureNeedsUpdateAfterLoad()` keeps features queued when their creation
  frame is the restored frame or the next resumed frame. This covers the
  checkpoint boundary where save captures after frame `N` and replay resume
  advances into `N + 1`.
- `PathManager` finalization now clears empty registry entities after destroying
  real path/search components, preserving the final active-entity log as a leak
  signal without changing runtime restore behavior.

Verified evidence:

- Build: `.\scripts\build-recoil-synccheck-windows.ps1 -CompileOnly` exited
  `0` after the QTPFS search-state and cleanup changes.
- Fresh seeded fixture:
  `.cache/replay-timeline-synctest/run_20260610_155131`, seed
  `2091638182`, recorded digest `v2gwCY6m6aBluwoahUyYJw==` for frames
  `0..419`, bundle
  `write_episode_0/demos/rcp_e0544cfe.replay-checkpoints`.
- Bundle exactness check during recording reported checkpoint frames
  `90`, `180`, `270`, and `360` as exact.
- Strict LuaRules target 90:
  `.cache/replay-checkpoint-smoke/strict_luarules_target90_seed2091638182_after_qtpfs_search_state_infolog.txt`
  restored `SaveFrame=90` from request frame `211`, resumed to frame `421`,
  and matched digest `MavJ/YTMPnp++3XqSnAbwQ==` for frames `90..419`.
- Strict LuaRules target 180:
  `.cache/replay-checkpoint-smoke/strict_luarules_target180_seed2091638182_after_qtpfs_search_state_infolog.txt`
  restored `SaveFrame=180` from request frame `301`, resumed to frame `421`,
  and matched digest `kLVEK2n19naL7+R3vQjjWA==` for frames `180..419`.
- Strict LuaRules target 270:
  `.cache/replay-checkpoint-smoke/strict_luarules_target270_seed2091638182_after_qtpfs_search_state_infolog.txt`
  restored `SaveFrame=270` from request frame `361`, resumed to frame `421`,
  restored one ready QTPFS path search, and matched digest
  `j1VvhhtzpLCypcFkAODfBg==` for frames `270..419`.
- Strict LuaRules target 360:
  `.cache/replay-checkpoint-smoke/strict_luarules_target360_seed2091638182_after_qtpfs_search_state_infolog.txt`
  restored `SaveFrame=360` from request frame `401`, resumed to frame `421`,
  and matched digest `ixH/QnRdi7c+oYs7psxq9A==` for frames `360..419`.
- Strict BAR LuaUI replay-widget restore on the same fixture also passed without
  `-LuaRulesSelfTest`; the logs loaded `gui_replaybuttons.lua` before and after
  checkpoint restore:
  - target 90 evidence
    `.cache/replay-checkpoint-smoke/strict_luaui_target90_seed2091638182_after_qtpfs_search_state_infolog.txt`
    matched `MavJ/YTMPnp++3XqSnAbwQ==` for `90..419`.
  - target 180 evidence
    `.cache/replay-checkpoint-smoke/strict_luaui_target180_seed2091638182_after_qtpfs_search_state_infolog.txt`
    matched `kLVEK2n19naL7+R3vQjjWA==` for `180..419`.
  - target 270 evidence
    `.cache/replay-checkpoint-smoke/strict_luaui_target270_seed2091638182_after_qtpfs_search_state_infolog.txt`
    restored one ready QTPFS path search and matched
    `j1VvhhtzpLCypcFkAODfBg==` for `270..419`.
  - target 360 evidence
    `.cache/replay-checkpoint-smoke/strict_luaui_target360_seed2091638182_after_qtpfs_search_state_infolog.txt`
    matched `ixH/QnRdi7c+oYs7psxq9A==` for `360..419`.
- Forward BAR replay timeline UI smoke on the same fixture passed with
  `StartFrame=30`, `TargetFrame=181`, and `QuitFrame=421`. Evidence
  `.cache/replay-timeline-smoke/run_20260610_155131_forward181_fullsync_after_qtpfs_search_state_infolog.txt`
  loaded `gui_replaybuttons.lua`, reached target frame `181`, continued to
  frame `421`, and matched full replay digest `v2gwCY6m6aBluwoahUyYJw==` for
  frames `0..419`.
- The current smoke logs end with `~PathManager: 0 entities still active!`
  after clearing checkpoint allocator placeholders, so the earlier shutdown
  `43 entities still active` noise is resolved.
- Owner-local exact-save seam added:
  `test/engine/System/LoadSave/testReplayCheckpointSavePlanner.cpp` builds
  `test_ReplayCheckpointSavePlanner.exe` and exercises frame-addressed save
  request planning without booting a map. It asserts current-frame filenames,
  active recording bundle filenames, overwrite args, immediate create-save
  callback dispatch, and callback failure propagation. Production still passes
  `FileSystem::EnsurePathSepAtEnd` into the planner, so bundle path
  normalization stays anchored to the existing engine helper. Verified with
  `test_ReplayCheckpointSavePlanner.exe --success`: 14 assertions in 4 test
  cases. The full Windows SYNCCHECK compile-only build also exited `0` after
  adding `ReplayCheckpointSavePlanner.cpp` to the engine source list.
- Owner-local feature queue seam added:
  `test/engine/Sim/Features/testFeatureUpdateQueuePolicy.cpp` builds
  `test_FeatureQueuePolicy.exe` and exercises the restored feature update-queue
  predicate without booting the sim. It locks down the checkpoint-boundary case
  that stationary features created at restored frame `N` or resumed frame
  `N + 1` must remain queued for the post-restore movement notification, while
  older/later stationary on-ground features are pruned. It also preserves the
  dynamic cases (`deleteMe`, move control, velocity, smoke/fire, geothermal,
  and off-ground). Verified with `test_FeatureQueuePolicy.exe --success`: 11
  assertions in 3 test cases. The full Windows SYNCCHECK compile-only build
  exited `0` after adding `FeatureUpdateQueuePolicy.cpp` to `engineSim`.
- Owner-local QTPFS search-envelope seam added:
  `test/engine/Sim/Path/QTPFS/testPathSearchReplayCheckpointState.cpp` builds
  `test_QTPFSSearchState.exe` and exercises the replay checkpoint
  `PathSearch` snapshot/apply policy without booting a map or path manager. It
  asserts capture of path/search entity IDs, component kind, team/path type,
  process-ready state, raw/synced/partial/repair/waiting flags, connectivity
  flags, and full/partial result flags. It also locks down the restore rule that
  `initialized=true` is only reapplied when the serialized search is also a
  ready `ProcessPath`; queued-but-not-ready searches must not resume as
  initialized. Verified with `test_QTPFSSearchState.exe --success`: 46
  assertions in 3 test cases. The full Windows SYNCCHECK compile-only build
  exited `0` after adding `PathSearchReplayCheckpointState.cpp` to `engineSim`.
- Owner-local QTPFS path-payload seam added:
  `test/engine/Sim/Path/QTPFS/testPathReplayCheckpointState.cpp` builds
  `test_QTPFSPathState.exe` and exercises replay checkpoint capture/apply of
  serializable `IPath` payloads without booting a map or path manager. It covers
  owner markers, path IDs/counters, hashes, radius, bounding box, goal position,
  search time, path type, synced/full/partial/raw flags, point arrays, and node
  arrays including net points, node bounds, path-point indices, and bad-node
  flags. `PathManager` still owns entity/component restoration, owner lookup,
  shared-path caches, search refs, and the existing checkpoint serialization
  order; the helper only owns the payload invariant. Verified with
  `test_QTPFSPathState.exe --success`: 114 assertions in 2 test cases. The full
  Windows SYNCCHECK compile-only build exited `0` after adding
  `PathReplayCheckpointState.cpp` to `engineSim`.

### Checkpoint-Owned Forward Timeline Jump And Replay Pause Preservation

Bug shapes:

- BAR replay timeline forward clicks still used direct `skip f<target>`, so the
  visual timeline could move forward only through the old speedup/skip path
  instead of the same checkpoint-owned jump path as rewind.
- The checkpoint self-test queued `pause 1` and `replaycheckpoint load` together.
  In hosted-demo replay mode, the visible "paused the demo" state is owned by
  local replay pause state, not by synced `gs->paused`. Capturing only
  `gs->paused` made the engine restore marker report `paused 0` even though the
  demo was functionally paused; writing replay pause back into `gs->paused`
  later caused real post-resume desync.
- Restore/load can take long enough that a demo needs an intentional transition,
  but that transition must stay unsynced and configurable so it cannot pollute
  SYNCCHECK acceptance evidence.

Fixes:

- Manual BAR timeline clicks now prefer `Spring.LoadReplayCheckpoint(target)`
  when `ReplayTimelineCheckpointJumps=1` (default), with
  `/replaycheckpoint load <target>` only as a temporary fallback. The remaining
  advance is bounded to the selected checkpoint interval.
  `ReplayTimelineSelfTestUseCheckpoint=1` makes the timeline smoke exercise
  that same path by default, while `-LegacySkipSelfTest` keeps old `skip`
  behavior available as a separate baseline.
- `ReplayCheckpointHandler` captures a single replay-pause intent. Hosted-demo
  restore preserves `CGame::paused` and `GameServer::isPaused`; non-demo load
  paths still restore synced/server pause. `gs->paused` remains checkpoint-owned
  for hosted-demo replays.
- The BAR replay pause button now routes through the same
  `Spring.SetReplayPaused` helper used by pause-before-jump and paused catch-up,
  with the old `pause 0/1` command only as a fallback. This keeps manual
  pause-then-jump ownership aligned with the tested replay-pause seam.
- The checkpoint self-test now has `ReplayCheckpointSelfTestPauseBeforeLoad`.
  The paused path waits a short wall-clock beat after requesting pause before
  issuing `replaycheckpoint load`; the unpaused path intentionally sends no
  pause command before load or resume because hosted demo `pause 0` can toggle
  the replay into paused state. The engine restore marker and post-restore
  SYNCCHECK equivalence are the authority.
- The BAR timeline self-test now has `ReplayTimelineSelfTestPauseBeforeJump`
  and `ExpectedRestorePaused` coverage for the real UI jump path. Dispatch is
  driven from `DrawScreen` as well as `Update` so a paused replay still issues
  the checkpoint load, and the post-restore widget owns the final
  reached/screenshot/quit markers by requiring a new restore serial at widget
  load.
- BAR LuaUI starts a gated unsynced restore transition for manual checkpoint
  timeline jumps. It is controlled by `ReplayCheckpointTransition`, persists
  target/request timing plus a frame fallback through config so it can survive
  LuaUI reload, and uses DrawScreen-deferred screenshot capture for restored
  frames. Smoke configs disable it unless transition evidence is requested.
  `ReplayCheckpointTransitionPreloadMs/PreloadDraws` are opt-in only and
  default to `0`, because delaying checkpoint load can let the source replay
  frame advance before restore.
- `scripts/replay-timeline-demo-windows.ps1` now writes unique manual-demo
  evidence dirs and explicit checkpoint config when `-SpringExe` is supplied:
  checkpoint timeline jumps and bundle use are enabled, transition is enabled
  unless `-NoTransition`, preload delay is disabled, visual cleanup windows are
  explicit, and stale transition/pause-catchup state starts at zero. It waits
  for the engine process to exit before reading `infolog.txt`, prints a
  checkpoint/transition/checkpoint-marker/API summary, rejects
  `[ReplayTimelinePausedCatchup] failed`, and `-RequireCheckpointJump`,
  `-RequireCheckpointApi`, and `-RequireCheckpointMarkers` make the summary a
  failing evidence gate.

Verified evidence:

- Build: `.\scripts\build-recoil-synccheck-windows.ps1 -CompileOnly` exited
  `0` after the pause-owner helper change.
- Fixture `.cache/replay-timeline-synctest/run_20260611_002746`, duration
  `1500`, recorded digest `uQ0WvLU2pI3qQoUu+9aJGA==` for `0..1499`, bundle
  `write_episode_0/demos/rcp_e9b98e5e.replay-checkpoints`.
- Forward checkpoint smoke:
  `.\scripts\replay-checkpoint-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260611_002746 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -SaveFrame 90 -LoadFrame 30 -TargetFrame 1200 -ResumeFrame 1501 -RequireSyncChecksumRestore -RequirePostRestoreSyncHash -RequireSynctestMarkers -TimeoutSeconds 360 -RestoreTimeoutSeconds 30`.
- Result restored request frame `30`, target frame `1200`, checkpoint frame
  `1170`, current frame `1170`, `paused=1`, and bounded catch-up span `30`.
  It resumed to frame `1501` with `paused=0`.
- Post-restore sync hash `WhPFMVwjoF/c0wdwHRYjHw==` covered frames
  `1170..1499` and matched the recorded replay. Evidence log:
  `.cache/replay-checkpoint-smoke/run_20260611_002746/write/infolog.txt`.
- After adding the LuaUI transition seam, `luac -p` passed for
  `luaui/Widgets/gui_replaybuttons.lua` and `luarules/gadgets/dbg_synctest.lua`;
  the same forward checkpoint smoke passed again with the transition disabled.
- Transition-enabled visual smoke:
  `.\scripts\replay-checkpoint-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -SaveFrame 90 -LoadFrame 390 -TargetFrame 180 -ResumeFrame 600 -RequireSyncChecksumRestore -RequireSynctestMarkers -CaptureScreenshots -EnableTransition -OutputSuffix transition_default_hold_20260611 -TimeoutSeconds 240 -RestoreTimeoutSeconds 120`.
  Evidence log:
  `.cache/replay-checkpoint-smoke/run_20260610_223907_transition_default_hold_20260611/write/infolog.txt`.
  It required `[ReplayCheckpointTransition] start`, restored `390 -> 180`,
  captured deferred `restored`/`resume-start` screenshots at restored frame
  `180`, logged `resume-deferred` before unpausing, skipped `1500` saved
  decals, resumed to `600` with `paused=0`, and found no
  desync/checksum-mismatch patterns.
- BAR timeline transition visual smoke:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 390 -TargetFrame 180 -QuitFrame 210 -CaptureScreenshots -EnableTransition -RequireCheckpointRestore -RequireCheckpointApi -OutputSuffix timeline_transition_visual_api_20260611 -TimeoutSeconds 420`.
  Evidence log:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_transition_visual_api_20260611/write/infolog.txt`
  plus screenshots in that run's `write/screenshots/`. This proves the real
  BAR timeline jump path can enable the gated time-machine overlay while still
  using `Spring.LoadReplayCheckpoint(180)`: transition start `target=180
  request=390`, API load accepted, checkpoint `180`, `0` catch-up, no command
  fallback, and GUI Shader `rects=0` at reached/post-target. The
  reached/post-target screenshots visibly show `TIME MACHINE ONLINE`, keep the
  raised timeline visible, and do not show the old full-screen sheets or
  imploding restore geometry. This is visual/demo evidence; SYNCCHECK authority
  remains with the long visual+sync runs.
- Unpaused forward checkpoint smoke:
  `.\scripts\replay-checkpoint-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260611_002746 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -SaveFrame 90 -LoadFrame 30 -TargetFrame 1200 -ResumeFrame 1501 -RequireSyncChecksumRestore -RequirePostRestoreSyncHash -RequireSynctestMarkers -RestoreUnpaused -TimeoutSeconds 360 -RestoreTimeoutSeconds 30`.
- Result restored request frame `37`, target frame `1200`, checkpoint frame
  `1170`, engine frame `1170`, current frame `1171`, `paused=0`, bounded
  catch-up span `30`, and resumed to frame `1501` with `paused=0`. The same
  digest `WhPFMVwjoF/c0wdwHRYjHw==` matched for `1170..1499`. Evidence log:
  `.cache/replay-checkpoint-smoke/run_20260611_002746_unpaused/write/infolog.txt`.
- After the visual-cleanup fixes, the same unpaused smoke passed again. The log
  shows the engine cleanup marker at frame `1170`, `DecalsGL4` skipped `15`
  saved decals during the cleanup window, and the previous `Unit Repeat Icons`
  stale `UnitID` call-in plus `Ecostats` nil-rect `DrawScreen` failure are no
  longer present.
- Forward BAR timeline checkpoint smoke:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260611_002746 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 30 -TargetFrame 1200 -QuitFrame 1501 -RequireSynctestMarkers -RequireSyncHash -OutputSuffix timeline_forward_cp_20260611 -TimeoutSeconds 420`.
  It logged `[ReplayTimelineTest] checkpoint-request`, resolved target `1200`
  to checkpoint `1170`, rejected `skip-request`, and matched digest
  `WhPFMVwjoF/c0wdwHRYjHw==` for frames `1170..1499`.
- After the transition dispatch refactor, the same forward BAR timeline path
  passed again with transition disabled:
  `.cache/replay-timeline-smoke/run_20260611_002746_timeline_forward_cp_after_transition_queue_20260611/write/infolog.txt`.
  It preserved the `1200 -> 1170` checkpoint resolution, `30`-frame catch-up,
  and digest `WhPFMVwjoF/c0wdwHRYjHw==`.
- The matching backward BAR timeline path also passed after the dispatch
  refactor:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_cp_after_transition_queue_20260611/write/infolog.txt`.
  It preserved the `390 -> 180` request, checkpoint `180`, `0`-frame
  catch-up, and digest `YYyR2AD+hpZsXxod+hQ4nA==`.
- Manual demo launcher required-jump smoke:
  `.\scripts\replay-timeline-demo-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -QuitFrame 180 -OutputSuffix demo_launcher_require_jump_wait_20260611 -WindowWidth 1280 -WindowHeight 720 -RequireCheckpointJump`.
  The log
  `.cache/replay-timeline-synctest/run_20260610_223907/replay_timeline_demo_write_demo_launcher_require_jump_wait_20260611/infolog.txt`
  validates launcher wiring and the post-run evidence gate: the report counted
  `selftest_checkpoint_requests=1`, `restore_resolutions=1`,
  `restored_checkpoints=1`, `transition_starts=1`, `skip_requests=0`,
  `desync_or_mismatch_patterns=0`, and `max_catchup_span=0`; target `180`
  restored to checkpoint `180`, sync-check checksum `31d9f94b` restored, and
  `759` saved decals were skipped during cleanup. This is launcher evidence,
  not a substitute for the final hand-click demo.
- After the GUI Shader cleanup and stricter launcher evidence parser, the same
  launcher gate passed with
  `-OutputSuffix demo_launcher_evidence_parser_20260611`. The summary counted
  `selftest_checkpoint_requests=1`, `restore_resolutions=1`,
  `restored_checkpoints=1`, `transition_starts=1`,
  `guishader_cleanups=1`, `skip_requests=0`,
  `desync_or_mismatch_patterns=0`, `lua_error_patterns=0`, and
  `max_catchup_span=0`; target `180` restored to checkpoint `180`.
- The launcher evidence parser now has a synthetic validation seam: a good log
  with checkpoint request, restore resolution, restored marker, transition
  start/draw, checkpoint marker load, API load, and GUI Shader cleanup is
  accepted, while a log containing `[ReplayTimelinePausedCatchup] failed` is
  rejected. This validates the manual-demo evidence gate without opening an
  interactive demo window.
- Backward BAR timeline checkpoint smoke:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 390 -TargetFrame 180 -QuitFrame 3001 -RequireSynctestMarkers -RequireSyncHash -OutputSuffix timeline_backward_cp_20260611 -TimeoutSeconds 900`.
  It resolved target `180` to checkpoint `180`, caught up `0` frames, and
  matched digest `YYyR2AD+hpZsXxod+hQ4nA==` for frames `180..2999`.
- Paused BAR timeline checkpoint smoke:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 390 -TargetFrame 180 -QuitFrame 180 -CaptureScreenshots -RequireCheckpointRestore -PauseBeforeJump -ExpectedRestorePaused 1 -OutputSuffix timeline_backward_paused_restore_serialguard_20260611 -TimeoutSeconds 360`.
  Evidence log
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_paused_restore_serialguard_20260611/write/infolog.txt`
  shows the UI path requested pause, dispatched a checkpoint jump, restored
  checkpoint `180`, and reported `paused=1` in both the engine restore marker
  and timeline reached marker. It saved `before-jump`/`reached` screenshots, and
  the reached capture stayed visibly paused without the old magenta/green sheet
  artifacts. Earlier paused harness attempts exposed two race shapes: `Update`
  alone did not reliably run while paused, and the pre-reload widget could mark
  `reached` before losing pending screenshot/quit state across LuaUI reload.
  This run is pause-invariant evidence; long forward/backward SYNCCHECK smokes
  remain the sync authority.
- Unpaused BAR timeline checkpoint smoke:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 30 -TargetFrame 1200 -QuitFrame 1200 -CaptureScreenshots -RequireCheckpointRestore -ExpectedRestorePaused 0 -OutputSuffix timeline_forward_unpaused_state_summary_20260611 -TimeoutSeconds 420`.
  Evidence log
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_unpaused_state_summary_20260611/write/infolog.txt`
  shows checkpoint request `30 -> 1200`, restored checkpoint `1170`, bounded
  catch-up `30`, engine restore `paused=0`, timeline reached `paused=0`, no
  legacy skip request, and GUI Shader `rects=0` at the reached screenshot. This
  proves the current marker/harness covers the unpaused half of the pause
  invariant; the long forward visual+SYNCCHECK run remains the sync authority.
- Paused forward resume exposed the replay/synced-pause ownership bug:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_paused_resume_api_sync_20260611/write/infolog.txt`
  restored `390 -> 1200` through checkpoint `1170`, reached the paused target,
  then desynced after `resume-after-reached`. First warning was at frame `1260`;
  post-restore digest was `bcyEXP3hxUKUfYopF3sKJg==`. This was not a visual-only
  issue: the bug was preserving replay pause by mutating synced `gs->paused`.
- Current paused forward API evidence after the ownership fix and precise
  catch-up state machine:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_paused_resume_precise_catchup_sync_20260611/write/infolog.txt`.
  BAR used `Spring.LoadReplayCheckpoint(1200)`, resolved checkpoint `1170`,
  prepared catch-up speed while still paused, caught up exactly `1170 -> 1200`,
  restored the previous replay speed, reached `current=1200 target=1200
  paused=1`, resumed at `1200`, and matched digest
  `oh6rmUltongIJMqlhqyPRg==` for frames `1170..2999`. Screenshots are visually
  clean and keep the timeline visible.
- The timeline smoke verifier now rejects paused frame drift instead of allowing
  a small tolerance. Fresh strict runs:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_paused_resume_exact_assert_3001_20260611/write/infolog.txt`
  and
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_paused_resume_exact_assert_20260611/write/infolog.txt`
  both used the Lua API, preserved `paused=1`, reached and resumed at the exact
  targets (`1200` and `180`), and matched digests
  `oh6rmUltongIJMqlhqyPRg==` (`1170..2999`) and
  `YYyR2AD+hpZsXxod+hQ4nA==` (`180..2999`). A same-path forward run with
  `QuitFrame 3000` reached/resumed exactly but failed the harness because the
  SYNCCHECK end marker needed frame `3001`; use `QuitFrame 3001` for full-run
  timeline digest proof on this fixture.

### LuaUI Visual Cleanup After Restore

Bug shapes:

- Restoring a checkpoint reloads unsynced LuaUI, but widget-local VBOs,
  render-to-texture state, and saved visual caches can still refer to the
  pre-restore world.
- `gfx_decals_gl4.lua` restored saved decals after checkpoint restore, which
  matched the user-reported "imploding" and sheet-like visual corruption risk.
- `gui_unit_repeat_icon.lua` handled a stale `UnitCommand` for non-existing unit
  `31719`, then called into VBO instance data paths that queried the missing
  unit and removed the widget.
- `gui_ecostats.lua` could keep `uiTex`/`uiBgTex` while `areaRect` was empty,
  then call `gl.TexRect(nil, ...)` from `DrawScreen`.
- `gfx_guishader.lua` could keep or quickly re-register large blur masks while
  LuaUI was still settling after restore. The short timeline screenshot smoke
  showed stale blurred minimap/player-list rectangles even after the magenta
  sheet and decal corruption were gone.
- `gui_defenserange_gl4.lua` and `gui_attackrange_gl4.lua` could rebuild large
  stencil-filled range VBOs immediately after checkpoint restore. On the old
  visual fixture this looked like magenta/orange/green sheets and imploding
  geometry after the jump; guarding only the sensor-range widgets did not fix
  it, so the owner was the range-ring GL4 state rather than the sensor stencils
  alone.

Fixes:

- `CGame::LoadReplayCheckpoint()` now emits a narrow unsynced cleanup marker:
  `ReplayCheckpointRestoreSerial`, restore/request frames, and
  `ReplayCheckpointVisualCleanupUntilFrame`. The engine does not own widget
  cleanup policy.
- `gfx_decals_gl4.lua` skips restoring saved decals while the cleanup marker is
  active.
- `gui_unit_repeat_icon.lua` clears repeat-icon VBO state once per restore
  serial and ignores stale unit visibility/command callbacks during the cleanup
  window. Invalid units are forgotten in Lua tables without popping a VBO
  instance that may query stale engine unit data, then visible units are
  refreshed from `WG.unittrackerapi` after the cleanup window closes.
- `gui_ecostats.lua` validates `areaRect` before render-to-texture and screen
  blending calls.
- `gfx_guishader.lua` now clears and suppresses blur rect/dlist state during a
  BAR-local cleanup window after replay checkpoint restore. The window is
  configurable through `ReplayCheckpointGuishaderCleanupFrames` and defaults to
  `90` frames; smoke/demo configs set it explicitly.
- `gui_defenserange_gl4.lua` and `gui_attackrange_gl4.lua` now clear their VBO
  instance tables and suppress range drawing/callback rebuilds once per restore
  serial during `ReplayCheckpointRangeCleanupFrames` (default `300` frames).
  Defense ranges rebuild from `WG.unittrackerapi` after the range cleanup
  window closes; attack ranges queue a normal selection refresh. The sensor
  range widgets also skip restore-window rebuilds, but that was not sufficient
  by itself.
- The smoke harness keeps the engine checkpoint-frame assertion strict, but
  allows a small LuaUI-observed frame tolerance for `-RestoreUnpaused`, where
  the simulation intentionally continues while LuaUI sees the restore marker.

Verified evidence:

- `luac -p` passed for `gui_unit_repeat_icon.lua`, `gui_ecostats.lua`,
  `gfx_decals_gl4.lua`, `gui_replaybuttons.lua`, and `dbg_synctest.lua`.
- `git diff --check` passed for the BAR, Recoil, script, and docs changes
  aside from expected CRLF warnings.
- Unpaused smoke
  `.cache/replay-checkpoint-smoke/run_20260611_002746_unpaused/write/infolog.txt`
  restored to checkpoint `1170`, resumed to `1501`, preserved `paused=0`, and
  matched digest `WhPFMVwjoF/c0wdwHRYjHw==` for `1170..1499`.
- Focused log scan found the cleanup marker and Decals skip, with no
  `LuaUI::RunCallInTraceback`, `LUA_ERRRUN`, `Error in UnitCommand`,
  `Non-existing UnitID`, `Error in DrawScreen`, or widget removal for
  Unit Repeat Icons/Ecostats after restore.
- Screenshot smoke on the original visual-regression fixture
  `.cache/replay-timeline-synctest/run_20260610_223907` restored request `390`
  to checkpoint `180`, preserved `paused=1`, skipped `1500` saved `DecalsGL4`
  decals, resumed to frame `3001` with `paused=0`, and matched digest
  `YYyR2AD+hpZsXxod+hQ4nA==` for frames `180..2999`. Evidence log:
  `.cache/replay-checkpoint-smoke/run_20260610_223907_fullhash_20260611/write/infolog.txt`.
  A focused negative scan found no LuaUI/LuaRules traceback, stale `UnitID`,
  `UnitCommand`, `DrawScreen`, widget-removal, desync, or checksum-mismatch
  patterns.
- `-CaptureScreenshots` saved `before-load`, `restored`, `resume-start`, and
  `resumed` images under
  `.cache/replay-checkpoint-smoke/run_20260610_223907_fullhash_20260611/write/screenshots/`.
  Against the six old glitch screenshots, sampled max magenta/green coverage
  dropped from `15.015%`/`24.442%` to `0.008%`/`0.103%` on the new captures.
- Transition overlay screenshots under
  `.cache/replay-checkpoint-smoke/run_20260610_223907_transition_default_hold_20260611/write/screenshots/`
  visibly show the `TIME MACHINE ONLINE` overlay and timeline at restored frame
  `180`;
  sampled magenta/green coverage on those overlay captures was
  `0.000%`/`0.030%`.
- Short timeline visual smoke after the guishader cleanup fix:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 175 -TargetFrame 180 -QuitFrame 210 -CaptureScreenshots -OutputSuffix timeline_visual_guishader_check_20260611 -TimeoutSeconds 300`.
  The updated verifier required the `[GUI Shader] Cleared blur state after
  replay checkpoint restore` marker, restored target `180` to checkpoint `180`,
  caught up `0` frames, rejected the legacy skip path, and saved three
  screenshots under
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_visual_guishader_check_20260611/write/screenshots/`.
  The `reached` and `post-target` captures no longer show the stale blurred
  rectangles from the previous short visual smoke. This is visual regression
  evidence only; the full `fullhash_20260611` smoke remains the SYNCCHECK
  authority.
- Full BAR timeline visual+SYNCCHECK smoke:
  `.\scripts\replay-timeline-smoke-windows.ps1 -RunDir .\.cache\replay-timeline-synctest\run_20260610_223907 -SpringExe .\RecoilEngine\build-windows\install\spring.exe -StartFrame 390 -TargetFrame 180 -QuitFrame 3001 -CaptureScreenshots -RequireSynctestMarkers -RequireSyncHash -OutputSuffix timeline_backward_visual_sync_20260611 -TimeoutSeconds 900`.
  Evidence log
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_visual_sync_20260611/write/infolog.txt`
  shows a UI checkpoint request at frame `390`, target `180` resolved to
  checkpoint `180`, `0` catch-up frames, no legacy skip request, GUI Shader
  cleanup through frame `270`, and digest
  `YYyR2AD+hpZsXxod+hQ4nA==` for frames `180..2999`. The `reached` screenshot
  at frame `188` keeps the timeline visible and is clear of the old
  magenta/green sheets, imploding visual state, and stale blurred UI panels.
- Late endgame overlay attribution and fix:
  `timeline_guishader_debug_20260611` added a gated
  `ReplayCheckpointGuishaderDebug` dump at screenshot capture time. At
  `timeline-post-target` it reported `rects=1:awards`, proving the large center
  blur was BAR `gui_awards.lua` endgame UI, not restore corruption.
  `ReplayTimelineSuppressEndAwards=1` now suppresses awards during replay
  timeline screenshot smoke and `gui_awards.lua` removes the `awards` rect
  whenever awards are not drawn. Follow-up
  `timeline_awards_suppressed_20260611` passed with `rects=0` at
  `timeline-post-target`; the screenshot no longer has the big center blur and
  `replay-timeline-smoke-windows.ps1` now rejects a post-target `awards` rect.
- Final combined backward BAR timeline visual+SYNCCHECK smoke after awards
  suppression:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_visual_sync_after_awards_20260611/write/infolog.txt`.
  This supersedes the earlier split evidence for the backward UI path: it
  requested checkpoint restore `390 -> 180`, restored checkpoint `180`, caught
  up `0` frames, rejected legacy skip, cleaned GUI Shader state through frame
  `270`, kept the timeline visible in screenshots, removed the late `awards`
  rect (`rects=0` at `timeline-post-target`), and matched digest
  `YYyR2AD+hpZsXxod+hQ4nA==` for frames `180..2999`.
- Final combined forward BAR timeline visual+SYNCCHECK smoke on the same
  fixture:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_visual_sync_after_awards_20260611/write/infolog.txt`.
  This proves the forward UI path no longer depends on direct replay skip:
  checkpoint request `30 -> 1200`, restored checkpoint `1170`, bounded catch-up
  `30` frames, no legacy skip, GUI Shader cleanup through frame `1260`,
  `rects=0` at both `timeline-reached` and `timeline-post-target`, visible
  timeline screenshots without the old sheet artifacts or late awards blur, and
  digest `oh6rmUltongIJMqlhqyPRg==` for frames `1170..2999`.
- API-gated full forward BAR timeline visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_visual_sync_api_20260611/write/infolog.txt`.
  This proves the new Lua API path for forward UI jumps: `api-load target=1200
  accepted=1`, no `command-load` fallback, checkpoint `1170`, bounded catch-up
  `30`, timeline reached `1200` with `paused=0`, `rects=0` at reached and
  post-target GUI Shader dumps, and digest `oh6rmUltongIJMqlhqyPRg==` for
  frames `1170..2999`.
- API-gated full backward BAR timeline visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_visual_sync_api_20260611/write/infolog.txt`.
  This proves the same Lua API path for rewind: `api-load target=180 accepted=1`,
  no `command-load` fallback, checkpoint `180`, bounded catch-up `0`, reached
  the target window with `paused=0`, `rects=0` at reached and post-target GUI
  Shader dumps, and digest `YYyR2AD+hpZsXxod+hQ4nA==` for frames `180..2999`.
- Manual-button pause seam smoke:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_pause_button_seam_20260611/write/infolog.txt`.
  `ReplayTimelineSelfTestPauseViaButton=1` routes pause-before-jump through the
  same helper used by the BAR replay pause button. The run logged
  `pause-api source=manual-button paused=1 accepted=1`, used
  `api-load target=180 accepted=1`, restored checkpoint `180`, reached
  `current=180 target=180 paused=1`, and resumed from that same target frame.
  This is pause-button ownership evidence; the full visual+SYNCCHECK runs above
  remain the replay-equivalence authority.
- Full forward manual-button pause + SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_pause_button_exact_sync_20260611/write/infolog.txt`.
  Command shape:
  `-StartFrame 390 -TargetFrame 1200 -QuitFrame 3001 -RequireCheckpointRestore -RequireCheckpointApi -RequireSynctestMarkers -RequireSyncHash -PauseBeforeJump -PauseViaButton -ResumeAfterReached -ExpectedRestorePaused 1`.
  It logged `pause-api source=manual-button paused=1 accepted=1`, resolved
  target `1200` to checkpoint `1170`, used `api-load target=1200 accepted=1`,
  prepared and ran bounded catch-up `1170 -> 1200` (`span=30`), reached and
  resumed at exact `current=1200 target=1200 paused=1`, and matched digest
  `oh6rmUltongIJMqlhqyPRg==` for frames `1170..2999`.
- The checkpoint and timeline smoke verifiers now reject the explicit failure
  families named by the active goal: `DESYNC WARNING`, sync-hash mismatch,
  replay/checksum mismatch, sync errors, keyframe differences, LuaRules/LuaUI
  call-in failures, DrawScreen/UnitCommand errors, and stale
  `Non-existing UnitID` restore errors. Timeline smoke also rejects checkpoint
  resolutions after the requested frame, so the selected checkpoint must be at
  or before the target before any catch-up span is accepted. This is harness
  coverage for future runs; the evidence above remains the current sync/visual
  authority.
- Harness rerun evidence for the new checkpoint-at-or-before-target gate:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_api_hardened_checkpoint_gate_20260611/write/infolog.txt`.
  The BAR timeline self-test required the Lua API, transition, screenshots, and
  checkpoint restore for `390 -> 180`; it resolved checkpoint `180`, used
  `api-load target=180 accepted=1`, made no `skip-request` or `command-load`
  fallback, reported catch-up span `0`, had GUI Shader `rects=0` at reached and
  post-target, and captured the raised timeline plus `TIME MACHINE ONLINE`
  overlay. This is visual/API verifier evidence, not final SYNCCHECK authority,
  because the old visual fixture still carries the known archive-copy warning.
- Focused no-transition visual smoke on the old glitch fixture showed the
  sensor-range-only cleanup was insufficient, then the range-GL4 cleanup removed
  the broken sheets:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_visual_cleanup_range_owner_20260611/write/infolog.txt`.
  It used `api-load target=180 accepted=1`, checkpoint `180`, catch-up `0`, no
  legacy skip/command fallback, and logged `Defense Range GL4 cleared range
  state` plus `Attack Range GL4 cleared range state` through frame `480`.
  The reached screenshot at frame `188` was clean; the frame `360` screenshot
  stayed inside the cleanup window and no longer showed the old polygon sheets.
- Past-window visual smoke verified the range widgets rebuild cleanly after the
  cleanup window:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_visual_cleanup_range_rebuild_20260611/write/infolog.txt`.
  Defense ranges rebuilt and attack ranges queued selection rebuild at frame
  `481`; the post-target screenshot at frame `543` showed normal battle
  rendering without the magenta/green sheet or imploding geometry artifacts.
  These two runs are visual/API regression evidence only; sync authority still
  comes from the SYNCCHECK runs on clean fixtures.
- Fresh current-worktree fixture recorded after the harness hardening:
  `.cache/replay-timeline-synctest/run_20260611_155032`, duration `1500`, seed
  `1776250575`, recorded digest `1ElWoXMT4nW/SjOg0lZhCA==` for `0..1499`, and
  checkpoint bundle `rcp_6111dfdf.replay-checkpoints` with `19` save files.
  The recording marker scan found no archive-copy warning.
- Fresh forward API/SYNCCHECK smoke on that fixture:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_api_sync_20260611/write/infolog.txt`.
  It required checkpoint restore, API load, synctest markers, and sync hash;
  target `1200` resolved to checkpoint `1170`, `api-load target=1200
  accepted=1`, no `command-load` fallback, no `skip-request`, bounded catch-up
  `30`, exact reached marker `current=1200 target=1200 paused=0`, and restored
  digest `rJ3pD3afQ5t3xjHCSl7AwQ==` matched recorded frames `1170..1499`.
- Fresh backward API/SYNCCHECK smoke on that fixture:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_fresh_api_sync_20260611/write/infolog.txt`.
  It required checkpoint restore, API load, synctest markers, and sync hash;
  target `180` resolved to checkpoint `180`, `api-load target=180 accepted=1`,
  no `command-load` fallback, no `skip-request`, catch-up span `0`, and
  restored digest `uwYRIjtUtKJgWnHDj29Piw==` matched recorded frames
  `180..1499`.
- Fresh paused/manual-button API/SYNCCHECK smoke on that fixture:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_pause_button_sync_20260611/write/infolog.txt`.
  It required checkpoint restore, API load, synctest markers, sync hash, manual
  pause before jumping, expected restored pause state `1`, and resume after the
  exact reached marker. The log shows `pause-api source=manual-button paused=1
  accepted=1`, target `1200` resolved to checkpoint `1170`, bounded catch-up
  `30`, target re-pause at `current=1200 target=1200 paused=1`, no
  `command-load` or `skip-request`, and restored digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` matched recorded frames `1170..1499`.
- Fresh forward visual+SYNCCHECK smoke after range cleanup:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_visual_sync_after_range_cleanup_20260611/write/infolog.txt`.
  It required checkpoint restore, Lua API, screenshots, synctest markers, and
  sync hash for `30 -> 1200`, resolved checkpoint `1170`, ran bounded catch-up
  `30`, reached exact `current=1200 target=1200 paused=0`, logged range
  cleanup through frame `1470` and range rebuild at frame `1471`, and matched
  digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for frames `1170..1499`.
  Reached/post-target screenshots were visually clean with the raised timeline
  visible, GUI Shader `rects=0`, and no legacy skip or command fallback.
- Fresh backward visual+SYNCCHECK smoke after range cleanup:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_fresh_visual_sync_after_range_cleanup_20260611/write/infolog.txt`.
  It required checkpoint restore, Lua API, screenshots, synctest markers, and
  sync hash for `1200 -> 180`, resolved checkpoint `180`, catch-up span `0`,
  reached the target window with `current=181 target=180 paused=0`, logged
  range cleanup through frame `480` and range rebuild at frame `481`, and
  matched digest `uwYRIjtUtKJgWnHDj29Piw==` for frames `180..1499`.
  Reached/post-target screenshots were visually clean with the raised timeline
  visible, GUI Shader `rects=0`, and no legacy skip or command fallback.
- Fresh paused forward visual+SYNCCHECK smoke after range cleanup:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_pause_visual_sync_after_range_cleanup_20260611/write/infolog.txt`.
  It used the manual pause-button helper, required checkpoint restore, Lua API,
  screenshots, synctest markers, sync hash, expected restored pause state `1`,
  and resume after the exact reached marker. The run resolved `1200` to
  checkpoint `1170`, caught up `30` frames, reached exact
  `current=1200 target=1200 paused=1`, cleaned/rebuilt range widgets through
  frame `1470/1471`, and matched digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for
  frames `1170..1499`. Reached/post-target screenshots were clean with the
  raised timeline visible and no legacy skip or command fallback.
- Fresh paused backward visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_fresh_pause_visual_sync_20260611/write/infolog.txt`.
  It used the manual pause-button helper, required checkpoint restore, Lua API,
  screenshots, synctest markers, sync hash, expected restored pause state `1`,
  and resume after the exact reached marker. The run resolved `180` to
  checkpoint `180`, catch-up span `0`, reached exact
  `current=180 target=180 paused=1`, resumed from frame `180`, rebuilt range
  widgets after the cleanup window, and matched digest
  `uwYRIjtUtKJgWnHDj29Piw==` for frames `180..1499`. The reached screenshot is
  visually dark at the early battle state but keeps the raised timeline visible,
  GUI Shader reports `rects=0`, and the run has no legacy skip or command
  fallback.
- Fresh transition v3 visual+SYNCCHECK smoke after range cleanup:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_transition_v3_visual_sync_20260611/write/infolog.txt`.
  It enabled `ReplayCheckpointTransition`, required checkpoint restore, Lua API,
  transition draw, screenshots, synctest markers, and sync hash for
  `390 -> 1200`. The log shows transition start `target=1200 request=390`,
  transition draw at frame `1170`, API load accepted, checkpoint `1170`,
  bounded catch-up `30`, exact reached
  `current=1200 target=1200 paused=0`, clean GUI Shader dumps with `rects=0`,
  range rebuild at frame `1471`, and digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for
  frames `1170..1499`. The reached screenshot visibly shows `TIME MACHINE
  ONLINE`, radial rings/spokes, and `FORWARD +810f`, keeps the raised timeline
  visible, and has no legacy skip or command fallback.
- Fresh transition text/screenshot gate after moving transition text to
  `gl.Text` with inline color codes:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_transition_gltext_color_sync_20260611/write/infolog.txt`.
  It required checkpoint restore, Lua API, checkpoint markers, transition draw,
  screenshots, synctest markers, and sync hash; restored `390 -> 1200` through
  checkpoint `1170`, bounded catch-up `30`, exact reached
  `current=1200 target=1200 paused=0`, and matched digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`. Focused scan found
  `api_loads=1`, `command_loads=0`, `skip_requests=0`,
  `paused_failures=0`, and no desync/checksum or Lua restore-error patterns.
  Reached screenshot `screen_2026-06-11_19-00-44-870.png` shows readable
  `TIME MACHINE ONLINE`, `FORWARD +810f`, rings/panel, and checkpoint ticks.
- Fresh checkpoint-marker visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_checkpoint_markers_visual_sync_20260611/write/infolog.txt`.
  This requires the new checkpoint availability marker from BAR:
  `[ReplayTimelineCheckpoints] loaded count=`. The run used
  `Spring.GetReplayCheckpoints()` to load `19` bundled frames (`first=90`,
  `last=1710`), used `Spring.LoadReplayCheckpoint(1200)`, restored checkpoint
  `1170`, caught up `30` frames, reached exact
  `current=1200 target=1200 paused=0`, drew the transition at frame `1170`,
  and matched digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for frames `1170..1499`.
  A focused negative scan found no desync, sync/checksum mismatch, Lua call-in,
  stale unit, legacy `skip-request`, or command-fallback markers. The reached
  screenshot shows the raised timeline with checkpoint tick marks, keeping the
  UI evidence tied to engine-owned checkpoint frames.
- Fresh paused catch-up settle-budget smoke:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_pause_settle_checks_sync_20260611/write/infolog.txt`.
  This covers the paused forward path after replacing pause/speed CPU-time
  settle fallbacks with explicit update-count budgets and a hard
  `[ReplayTimelinePausedCatchup] failed` marker. The run used the manual
  pause-button helper, API load, checkpoint `1170`, bounded catch-up `30`,
  exact target pause `current=1200 target=1200 paused=1`, resume from `1200`,
  screenshots with the raised timeline and checkpoint ticks visible, and digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`. A focused negative scan found
  no paused-catchup failure, desync, sync/checksum mismatch, Lua call-in, stale
  unit, legacy skip, or command fallback markers.
- Fresh strict manual-demo launcher gate:
  `.cache/replay-timeline-synctest/run_20260611_155032/replay_timeline_demo_write_demo_launcher_strict_api_markers_20260611/infolog.txt`.
  The launcher ran auto-quit with `-RequireCheckpointJump`,
  `-RequireCheckpointApi`, and `-RequireCheckpointMarkers`; the post-run summary
  counted `selftest_checkpoint_requests=1`, `restore_resolutions=1`,
  `restored_checkpoints=1`, `transition_starts=1`, `transition_draws=1`,
  `checkpoint_markers=2`, `api_loads=1`, `command_loads=0`,
  `skip_requests=0`, `paused_catchup_failures=0`,
  `desync_or_mismatch_patterns=0`, `lua_error_patterns=0`, and
  `max_catchup_span=30`. It restored target `1200` through checkpoint `1170`
  and loaded `19` timeline checkpoints (`first=90 last=1710`). This keeps the
  hand-demo launcher honest about API-owned jumps and visible checkpoint ticks;
  physical click feel still needs a real manual pass.
- Fresh screenshot-gated manual-demo launcher run:
  `.cache/replay-timeline-synctest/run_20260611_155032/replay_timeline_demo_write_demo_launcher_screenshot_gate_gltext_color_20260611/infolog.txt`.
  The new `-CaptureScreenshots` launcher gate requires timeline screenshot
  markers, deferred screenshot capture, GUI Shader reached debug dump, and at
  least two PNGs. This run reported `screenshot_requests=2`,
  `screenshot_captures=1`, `guishader_debug_dumps=2`, `api_loads=1`,
  `checkpoint_markers=2`, `command_loads=0`, and no bad Lua/sync patterns; it
  saved `screen_2026-06-11_18-55-02-604.png` with readable transition text.
- Fresh widget-click visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_click_path_visual_sync_20260611/write/infolog.txt`.
  The new `-ViaTimelineClick` path sets
  `ReplayTimelineSelfTestViaTimelineClick=1`, computes a timeline click point,
  and dispatches through `widget:MousePress`/`frame_from_timeline_x` before the
  checkpoint request. The run logged
  `timeline-click current=390 target=1200 click_frame=1200 ... active=1`, used
  API load, restored checkpoint `1170`, bounded catch-up `30`, exact reached
  `current=1200 target=1200 paused=0`, saved clean readable transition/tick
  screenshots including `screen_2026-06-11_19-30-26-352.png`, and matched digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`. No legacy skip, command
  fallback, paused-catchup failure, desync/checksum, or Lua error markers were
  reported by the smoke gate.
- Forward paused widget-click visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_click_path_paused_visual_sync_20260611/write/infolog.txt`.
  It combines `-ViaTimelineClick`, `-PauseViaButton`,
  `-ResumeAfterReached`, and `ExpectedRestorePaused=1`. The run logged
  `jump-dispatch current=390 target=1200 paused=1`,
  `timeline-click current=390 target=1200 click_frame=1200 ... active=1`,
  API load, checkpoint `1170`, bounded catch-up `30`, restored `paused 1`,
  exact `reached current=1200 target=1200 paused=1`,
  `resume-after-reached current=1200`, clean screenshots including
  `screen_2026-06-11_20-05-16-429.png`, and digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`.
- Backward widget-click visual+SYNCCHECK smoke:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_click_path_visual_sync_20260611/write/infolog.txt`.
  This uses the same `-ViaTimelineClick` path for `1200 -> 180`; it logged
  `timeline-click current=1200 target=180 click_frame=180 ... active=1`, used
  API load, restored checkpoint `180`, preserved `paused=0`, reached marker
  `current=181 target=180 paused=0` because the replay was running, saved clean
  rewind/tick screenshots including `screen_2026-06-11_19-43-46-384.png`, and
  matched digest `uwYRIjtUtKJgWnHDj29Piw==` for `180..1499`.
- Paused backward widget-click regression and fix:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_click_path_paused_visual_sync_20260611/write/infolog.txt`
  failed with `Restored checkpoint paused state 0 did not match expected 1`.
  Bug shape: the paused backward self-test started at frame `1200` with target
  `180`, so the generic `frame >= target` reached check fired before the delayed
  timeline click dispatched; `resume-after-reached` unpaused the replay, then
  restore correctly preserved that unpaused state. The fix prevents reached
  markers while `timelineSelfTestPendingJump` is true. Fixed evidence:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_click_path_paused_visual_sync_fixed_20260611/write/infolog.txt`
  logs `jump-dispatch current=1200 target=180 paused=1`,
  `timeline-click ... click_frame=180`, API load, checkpoint `180`,
  `restored ... paused 1`, exact `reached current=180 target=180 paused=1`,
  `resume-after-reached current=180`, clean screenshots including
  `screen_2026-06-11_19-56-58-937.png`, and digest
  `uwYRIjtUtKJgWnHDj29Piw==` for `180..1499`.
- Visual cleanup refresh regression: suppressing stale range/sensor VBO state is
  not enough if the widget never rebuilds after the cleanup window, and stale
  restore config must not activate cleanup before a new restore frame. The BAR
  sensor range widgets now require `restoreFrame <= currentFrame <=
  cleanupUntilFrame`, set a pending visible-unit refresh while cleanup is
  active, and rebuild from `WG.unittrackerapi.visibleUnits` after the window.
  The smoke/demo/checkpoint launch configs also reset restore serial/frame and
  cleanup-until keys. Evidence:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_click_visual_cleanup_refresh_rebuild_20260611/write/infolog.txt`
  shows checkpoint API load for `1200 -> 180`, stale decal/range cleanup at
  frame `180`, `Sensor Ranges Jammer/Radar/LOS rebuilt range state after visual
  cleanup` plus attack/defense rebuild at frame `482`, clean reached/post-target
  screenshots, no legacy skip or command fallback, and digest
  `uwYRIjtUtKJgWnHDj29Piw==` for `180..1499`.

Remaining seams:

- The load-completion handoff now uses `ReplayCheckpointRestoreSerial` plus
  `ReplayCheckpointRestoreTargetFrame` after engine restore. Paused catch-up now
  has explicit settle budgets and a harness-rejected failure marker; remaining
  timing cleanup should focus on self-test pause delays, preload exploration,
  and screenshot friendliness.
- Physical hand-demo clicks should still judge button feel, but fresh smokes now
  cover forward/backward and paused/unpaused timeline `MousePress` paths before
  the checkpoint request.
- Opt-in preload evidence
  `.cache/replay-checkpoint-smoke/run_20260610_223907_transition_preload_hold_20260611/write/infolog.txt`
  proved `preload` and `load-dispatch` markers, but advanced the source replay
  from frame `390` to `404` before dispatch. Keep preload disabled until pause
  ownership/load dispatch has a stronger API seam.
- Keep `run_20260610_223907` as a visual demo regression fixture. The full
  screenshot+SYNCCHECK smoke covers the automated LuaUI path; the checkpoint and
  timeline transition smokes cover automated overlay capture; and the timeline
  screenshot smoke now suppresses/checks the endgame awards overlay so
  post-target evidence remains about replay timeline visuals. Manual demo passes
  should still decide whether normal awards/endgame UI should be visible or
  suppressed.

Future seams:

- Add a QTPFS CREG/replay-checkpoint round-trip that restores a live path,
  `SearchModeIPath`, `PathSearchRef`, `ProcessPath`, and full/partial result
  flags, then asserts the next `SyncUpdatedPathsSystem` update produces the same
  path payloads and search hashes.
- Extend the checkpoint-recorder seam from request planning into a runtime or
  CREG-backed test that proves frame-addressed checkpoint files contain the
  requested `gs->frameNum`, not a later deferred-save frame.
- Extend the feature queue policy seam into a `CFeatureHandler` restore test
  that starts with stale, duplicate, invalid, and missing update-queue entries
  and proves `inUpdateQue` flags plus queue order are repaired after load.
- Keep the BAR LuaUI restore and forward timeline smokes as acceptance evidence,
  then extract smaller owner-local tests so UI smoke failures point to a narrow
  subsystem instead of the whole replay/checkpoint pipeline.
