# Replay Checkpoint Regression Tests

This document tracks regression cases for replay checkpoint restore. The goal is
to turn each desync we find into a focused test target, even if the current
engine shape requires smoke tests before smaller unit tests are practical.

Upstream design context and source links are summarized in
[`replay-checkpoint-upstream-notes.md`](replay-checkpoint-upstream-notes.md).

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
