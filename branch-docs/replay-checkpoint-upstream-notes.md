# Replay Checkpoint Upstream Notes

These notes summarize upstream Recoil/BAR discussions that are relevant to replay
checkpoint restore, timeline seek, and desync work. They are design context, not
evidence that the current checkpoint implementation is correct.

## Source Threads

- Recoil netcode overview:
  <https://recoilengine.org/articles/netcode-overview/#replays-and-saves>
- RecoilEngine #1388, "A loaded game doesn't produce a correct replay":
  <https://github.com/beyond-all-reason/RecoilEngine/issues/1388>
- RecoilEngine #1388 comment with approach summary:
  <https://github.com/beyond-all-reason/RecoilEngine/issues/1388#issuecomment-3707914267>
- RecoilEngine discussion #2704, "Game snapshots/save/load for quick catch-up
  or replay seek":
  <https://github.com/beyond-all-reason/RecoilEngine/discussions/2704>
- RecoilEngine #1179, "Add `spTurnReplayIntoNormalGame()` to turn replay into a
  normal game":
  <https://github.com/beyond-all-reason/RecoilEngine/issues/1179>
- RecoilEngine #350, "Replay: pause time flow":
  <https://github.com/beyond-all-reason/RecoilEngine/issues/350>

## Restart Cleanup 2026-06-22

The current shareable branch should be reviewed as Timeline Core: engine replay
checkpoint APIs, timeline checkpoint ticks, checkpoint-owned jumps, pause
preservation, bounded catch-up, and SYNCCHECK/sync-hash proof. BAR transition
overlay work, screenshot gates, GUI-shader/range cleanup windows, and broad
unsynced visual cleanup experiments were archived on
`replay-archive/2026-06-22-unsynced-gui-experiments` at `d3445bd20d20`.
Historical notes below still explain why those experiments existed, but they
should not be treated as active upstream scope.

## Main Takeaways

- Recoil's lockstep model sends player inputs, not simulation state. A replay is
  a copy of the network queue. A save is currently an incomplete snapshot, so
  replaying identical inputs from a loaded save can diverge from an uninterrupted
  game.
- The current replay checkpoint experiment is closest to the "perfect sync-safe
  saves" family from RecoilEngine #1388. That is the right family for true
  backwards replay seek, but upstream explicitly calls out size, save-time
  stutter, and long-tail desync hunting as the hard parts.
- Replay history must remain authoritative. If a save/load path creates a state
  that no longer matches the original replay, any resulting replay can become
  misleading. Checkpoints should therefore be treated as accelerators back into
  the immutable replay, not as a new history.
- Pathing is a known central risk. Upstream notes that pathing state is not fully
  saved, pathing updates are delayed, and simply capturing current pathing data
  can require very large state plus queued work. The local QTPFS/path ID findings
  are consistent with this warning.
- Storage and transfer optimization is a later stage. Discussion #2704 gives
  practical reasons to care about snapshot size, upload time, and stutter, but
  correctness still comes first: a smaller checkpoint that resumes into a desync
  is not useful.

## Approach Tradeoffs From Upstream

Replay-as-save:

- Let users run a replay to an interesting point, stop replay command injection,
  and continue as a normal game. This is related to RecoilEngine #1179.
- Advantage: likely simpler and useful independently.
- Cost: users still wait for replay catch-up, games need clear UX, and takeover
  semantics are awkward because other teams' future replay commands no longer
  match the changed world. #1179 discussion suggests game-side control hooks,
  and possibly a save/relaunch flow, for assigning controllers or AIs.

Perfect sync-safe saves or replay-attached keyframes:

- Store enough state to restore frame `R`, then resume from replay inputs at
  `R` without divergence. This is the path needed for real replay rewind and
  timeline jumps without hidden speedup.
- Advantage: matches the desired user experience.
- Cost: checkpoint payloads may be large, save operations may stutter, and every
  omitted state owner becomes a possible desync source.

Reset-on-save:

- Make the live game drop or simplify the same state that save/load omits, then
  record that event in the replay so the save is "perfect by comparison".
- Advantage: may avoid serializing some hard transient state.
- Cost: raises authority and multiplayer policy questions, mutates the live
  game, may cause visible bumps, does not compose cleanly when already watching a
  replay, and still requires ongoing knowledge of what state is omitted.

## Local Implications

- The first milestone should remain narrow: restore to a bundled checkpoint,
  keep the viewer paused, position the demo reader at the restored frame, resume,
  and match original sync hashes from that point onward.
- Success must be measured by sync equivalence, not by the UI reaching the right
  frame or by the bundle verifying on disk.
- Every desync should produce a small note in
  `RecoilEngine/branch-docs/replay-checkpoint-regression-tests.md`, including
  the subsystem, observed divergence, attempted fix, and eventual test seam.
- Incremental upstream contributions are likely easiest to review when they are
  independent of the full replay-bundle experiment: missing CREG fields,
  post-load invariants, pathing restore fixes, frame-accounting cleanup, and
  deterministic smoke fixtures.
- The 2026-06-09 QTPFS work suggests separable review chunks if the POC proves
  correct: a small diagnostic seam for path-manager restore signatures, a
  QTPFS-local checkpoint snapshot/restore seam, the node-layer rebuild hook, and
  deterministic smoke fixtures. Keep these framed as subsystem-local invariants
  rather than replay-UI behavior.
- Compression, Arrow-like columnar experiments, or `.ssf` format redesign should
  wait until restore/resume sync equivalence is proven on representative
  fixtures. After correctness is green, storage work can optimize against the
  explicit performance and size targets.

## Open Questions

- Which synced state owners are still omitted from `.ssf`/CREG, and which of
  those affect replay resume determinism?
- Can QTPFS restore preserve enough path/search/cache state without storing
  impractically large late-game pathing data?
- Should replay checkpoint bundles remain sidecars, become embedded replay
  chunks, or support both through a manifest contract?
- What engine API should expose checkpoint availability and restore requests to
  BAR Lua once the C++ path is proven?
- How should sim-frame seek points relate to wall-clock replay presentation,
  especially around pauses and pregame timekeeping from RecoilEngine #350?

## 2026-06-09 QTPFS Node-Layer Outcome

- The allocator/path snapshot work aligned frame-91 QTPFS path entities but did
  not solve sync equivalence. Later evidence showed matching request and chain
  state with divergent `PathSearch` results, which narrowed the root cause to
  stale QTPFS node-layer graph state after restore.
- The current fix keeps ownership local: `CGame::LoadReplayCheckpoint()` calls
  `IPathManager::RebuildReplayCheckpointNodeLayersForLoad()` after CREG and
  quadfield restore, while QTPFS rebuilds node layers, map-damage trackers,
  path speed-mod state, and checksums from the restored map/blocking state.
- Latest verified fixture:
  `.cache/replay-timeline-synctest/run_20260609_110541`, bundle
  `write_episode_0/demos/rcp_4c3b3d19.replay-checkpoints`.
- Scripted strict smoke passed:
  `.cache/replay-checkpoint-smoke/strict_resume181_after_qtpfs_rebuild_console.txt`
  with `SaveFrame=90 LoadFrame=121 TargetFrame=90 ResumeFrame=181`.
- LuaUI replay-widget checkpoint smoke passed:
  `.cache/replay-checkpoint-smoke/strict_luaui_resume181_after_qtpfs_rebuild_console.txt`
  with the same restore/resume window.
- BAR replay timeline forward smoke passed:
  `.cache/replay-timeline-smoke/run_20260609_110541_forward181_fullsync_console.txt`
  with `StartFrame=30 TargetFrame=181 QuitFrame=421`, reproducing digest
  `xPXHk2p0w3oXfN+miaWxTg==` for frames `0..419`.

## 2026-06-09 Smooth-Mesh Restore Outcome

- The later checkpoint window (`SaveFrame=180 LoadFrame=241 TargetFrame=180
  ResumeFrame=300`) was not solved by QTPFS alone. After QTPFS owner preservation
  and feature update-queue repair, the remaining divergence came from
  `CReadMap::PostLoad()` repopulating `SmoothHeightMesh` with a full-map
  `MapChanged()` notification during checkpoint load.
- This is a subsystem ownership issue: replay checkpoints serialize
  `SmoothHeightMesh` mesh data and pending queues, so generic read-map postload
  must not enqueue a replacement smooth-mesh update workload for checkpoint
  restore.
- Current fix shape: `CGame::LoadReplayCheckpoint()` wraps CREG `LoadGame()`
  with a narrow `SmoothHeightMesh` checkpoint-load guard. Read-map, LOS,
  feature, and pathing post-load refreshes still run; only smooth-mesh
  `MapChanged()` queue mutation is suppressed while the serialized checkpoint is
  being restored.
- Historical strict scripted LuaRules smoke after the rebuild, using fixture
  `.cache/replay-timeline-synctest/run_20260609_142625` and bundle
  `write_episode_0/demos/rcp_f2ccdd39.replay-checkpoints`:
  `.cache/replay-checkpoint-smoke/strict_luarules_target90_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with digest `KkAnEdKkgjCguex7MIP6MQ==` for the contiguous restored
  frame span `90..419`, and
  `.cache/replay-checkpoint-smoke/strict_luarules_target180_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with digest `f4P+6UsMt7vWK3LSw0l++Q==` for the contiguous restored
  frame span `180..419`. Both runs restored the C++ SYNCCHECK checksum boundary
  and matched every emitted frame against the recorded replay artifact.
- Historical strict LuaUI replay-widget smoke for the earlier checkpoint on the
  same fixture:
  `.cache/replay-checkpoint-smoke/strict_luaui_target90_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with `SaveFrame=90 LoadFrame=121 TargetFrame=90 ResumeFrame=421`,
  restored C++ SYNCCHECK checksum `d1da9f70`, emitted digest
  `KkAnEdKkgjCguex7MIP6MQ==` for the contiguous restored frame span `90..419`,
  and matched every emitted frame against the recorded replay artifact.
- Historical strict LuaUI replay-widget smoke after the rebuild:
  `.cache/replay-checkpoint-smoke/strict_luaui_target180_resume421_post_restore_synchash_after_smooth_guard_final_build_infolog.txt`
  passed with the same fixture and bundle, `ResumeFrame=421`,
  `RequireSynctestMarkers`, `RequireSyncChecksumRestore`, and
  `RequirePostRestoreSyncHash`. The run restored the C++ SYNCCHECK checksum for
  checkpoint frame 180, restarted the Lua synctest checksum capture after Lua
  reload, emitted digest `f4P+6UsMt7vWK3LSw0l++Q==` for the contiguous restored
  frame span `180..419`, and matched every emitted frame against the recorded
  replay artifact. It produced no desync, sync-error, sync-hash mismatch, or
  keyframe-difference markers.
- Reproducibility correction: `run_20260609_142625` is not final acceptance
  evidence. Its cached `Beyond-All-Reason.sdd` was edited after recording to add
  the opt-in post-restore synctest restart, and the strict logs contain
  `Archive Beyond-All-Reason.sdd ... differs from the host's copy`.
- Clean recheck fixture `.cache/replay-timeline-synctest/run_20260609_175435`
  was re-recorded with the current BAR source and has recorded digest
  `Z2RQaqAOpChFwgKxO3ASnA==` for frames `0..419`. BAR forward replay smoke
  passed on that fixture with the same digest and no archive mismatch.
- Historical clean LuaRules checkpoint restore failure, superseded by later
  LocalModel, pause-ownership, checkpoint-API, and BAR timeline evidence:
  - target `90` restores SYNCCHECK boundary `653aa44d` but desyncs at frame
    `420`; post-restore digest `38sMPRU8iyZqbVrwYRrtDg==` over `90..419`
    first mismatches the recording at frame `391`.
  - target `180` restores SYNCCHECK boundary `50eefdc7` but desyncs at frames
    `240`, `300`, `360`, and `420`; post-restore digest
    `r7tmLxOACCQLllB5bM42aQ==` over `180..419` first mismatches at frame `222`.
- Treat the smooth-mesh guard as a necessary fix, not proof of completion. At
  that point, the next upstream-shareable seam was to localize the clean-fixture
  first mismatch frames with subsystem signatures before extracting PR-ready
  chunks; later sections record the resulting owner-local fixes and API/pause
  evidence.
- This suggests another separable review chunk: make serialized smooth-mesh
  restore a local invariant, with a small regression test that proves read-map
  postload cannot replace serialized smooth queues during replay checkpoint
  restore.

## 2026-06-10 LocalModel Bounding-Volume Outcome

- Fresh fixture `.cache/replay-timeline-synctest/run_20260610_104413` narrowed
  the next checkpoint-180 desync to projectile collision against unit-local
  model bounds. Projectile `10008` and unit `11277` matched on projectile state,
  script animation state, visible collidable pieces, and transforms, but the
  restored aggregate `LocalModel::boundingVolume` differed from the original.
- The ownership issue was `LocalModel::SetModel(model, false)`: this post-load
  path needs to reattach `S3DModelPiece` pointers and rebuild piece transforms,
  but it should not recompute aggregate bounds that CREG already restored from
  the checkpoint.
- The current fix preserves serialized `boundingVolume` and
  `needsBoundariesRecalc` while reattaching model pointers. That keeps the
  invariant subsystem-local: CREG restores exact state, `LocalModel` restores
  model references, and projectile collision consumes the restored state through
  its existing interfaces.
- Current strict scripted evidence passed:
  `.cache/replay-checkpoint-smoke/strict_target180_resume421_after_localmodel_bounds_unit_script_filter_infolog.txt`
  restored `SaveFrame=180` from request frame `301`, resumed to `421`, and
  matched post-restore digest `VkJSDt8CijCeDsL6mNY/2Q==` for frames `180..419`.
- Candidate PR/test seam: `test_LocalModel.exe` now covers the owner-local
  post-load boundary for `LocalModel::SetModel(model, false)`: synthetic
  same-piece-count models prove piece pointer reattachment does not replace the
  serialized aggregate collision bounds or dirty flag. A later CREG round trip
  can wrap the same invariant when a small load/save harness is available.
  Keep the focused projectile/local-model diagnostics gated by
  `ReplayCheckpointDebugDamageProjectileID`,
  `ReplayCheckpointDebugTargetQueryUnit`, and `ReplayCheckpointDebugCobUnitID`
  until that seam exists. `ReplayCheckpointDebugCobUnitID` now uses `-2` as the
  disabled default, `-1` only when all-unit COB/unit-script traces are requested,
  and non-negative values for a specific unit; this prevents a generic
  `ReplayCheckpointDebugSignatureFrame` run from enabling heavyweight COB traces
  by accident.
- The selector cleanup was verified after rebuild with
  `.cache/replay-checkpoint-smoke/debug_signature_frame271_target270_no_cob_default_after_diag_gate_infolog.txt`:
  the run restored target `270`, resumed to `272`, emitted compact signature and
  QTPFS markers, and produced `0` COB/unit-script trace matches with the default
  `ReplayCheckpointDebugCobUnitID=-2`.

## 2026-06-10 Exact Saves And QTPFS Search-State Outcome

- The next target-270 drift was two separate ownership issues. First, checkpoint
  artifacts must be written at the named frame: using the generic deferred
  `game->Save(...)` path allowed a file named for frame `N` to serialize a later
  frame. `ReplayCheckpointHandler::QueueSaveCurrentFrame()` now creates the
  `.ssf` immediately through `ILoadSaveHandler::CreateSave(...)`, keeping
  frame-addressed checkpoint ownership in the replay checkpoint recorder.
- Second, QTPFS path snapshots alone were not enough for later checkpoints. The
  original path had active or just-completed searches that `PathManager::Update()`
  consumed on the next frame. Restore now owns that state inside QTPFS: live path
  payloads, `SearchModeIPath` result snapshots, `PathSearchRef`, `ProcessPath`,
  search component kind, and full/partial result flags are serialized and
  restored before the registry is pruned.
- Fresh seeded fixture
  `.cache/replay-timeline-synctest/run_20260610_155131` with seed
  `2091638182` recorded digest `v2gwCY6m6aBluwoahUyYJw==` for frames `0..419`.
  The record script verified exact checkpoints at frames `90`, `180`, `270`,
  and `360`.
- Current strict scripted evidence:
  - target `90` restored from request frame `211`, resumed to `421`, and
    matched digest `MavJ/YTMPnp++3XqSnAbwQ==` for `90..419`.
  - target `180` restored from request frame `301`, resumed to `421`, and
    matched digest `kLVEK2n19naL7+R3vQjjWA==` for `180..419`.
  - target `270` restored from request frame `361`, restored one ready QTPFS
    search envelope, resumed to `421`, and matched digest
    `j1VvhhtzpLCypcFkAODfBg==` for `270..419`.
  - target `360` restored from request frame `401`, resumed to `421`, and
    matched digest `ixH/QnRdi7c+oYs7psxq9A==` for `360..419`.
- BAR LuaUI replay-widget acceptance evidence now covers the same fixture and
  checkpoint set using the default smoke driver. Each restore log loads
  `gui_replaybuttons.lua` before and after checkpoint restore, resumes to
  frame `421`, and matches the recorded replay from the restored frame onward:
  target `90` matches `MavJ/YTMPnp++3XqSnAbwQ==`, target `180` matches
  `kLVEK2n19naL7+R3vQjjWA==`, target `270` restores one ready QTPFS search and
  matches `j1VvhhtzpLCypcFkAODfBg==`, and target `360` matches
  `ixH/QnRdi7c+oYs7psxq9A==`.
- BAR replay timeline forward-jump acceptance evidence also passes on the same
  fixture: `StartFrame=30`, `TargetFrame=181`, `QuitFrame=421`,
  `gui_replaybuttons.lua` loaded, target frame `181` reached, and full replay
  digest `v2gwCY6m6aBluwoahUyYJw==` matched for `0..419`.
- Candidate PR/test seams: exact-frame checkpoint-save ownership in
  `ReplayCheckpointHandler` now has a first owner-local unit seam:
  `test_ReplayCheckpointSavePlanner.exe` covers current-frame filenames, active
  bundle paths, overwrite args, immediate create-save dispatch, and callback
  failure propagation through an injected create-save callback. The planner also
  injects directory normalization, while production keeps using
  `FileSystem::EnsurePathSepAtEnd`. Remaining upstreamable seams are a
  fuller QTPFS replay checkpoint search-state round-trip, a fuller
  `CFeatureHandler` queue-repair test, and a small `PathManager` finalization
  cleanup for empty restored allocator placeholders. The first QTPFS-local
  search-envelope unit seam is now `test_QTPFSSearchState.exe`: it covers
  capture/apply of the `PathSearch` replay flags and the important restore rule
  that `initialized` only comes back for ready `ProcessPath` searches. The first
  QTPFS-local path-payload seam is now `test_QTPFSPathState.exe`: it covers
  capture/apply of the serializable `IPath` payload while leaving owner lookup,
  entity/component restoration, shared caches, and checkpoint serialization
  order in `PathManager`. The feature creation-frame update-queue boundary now
  has a first pure unit seam:
  `test_FeatureQueuePolicy.exe` covers restored frame `N`/`N + 1` creation
  notifications, stale stationary features, missing restore-frame context, and
  the dynamic feature cases that must remain queued after load.
- Keep the UI smokes as branch acceptance evidence, but make the upstreamable
  correctness tests smaller and owner-local so regressions identify the exact
  save-frame, QTPFS search-state, feature queue, or cleanup boundary.

## 2026-06-11 Timeline Forward Jump And Pause Notes

- BAR timeline clicks now have the first checkpoint Lua API seam:
  `Spring.LoadReplayCheckpoint(frame)` wraps the same deferred hot-load request
  as `/replaycheckpoint load <frame>`. BAR prefers the API when
  `ReplayTimelineCheckpointJumps=1`; the command path remains a temporary
  fallback until the API surface is reviewed.
- Speedup/skip should remain bounded to the checkpoint interval only. The
  forward smoke now proves target `1200` restored to checkpoint `1170` and only
  caught up `30` frames, rather than skipping from the current replay frame.
- Hosted-demo replay pause is owned by local replay pause state:
  `CGame::paused` plus `GameServer::isPaused`. Synced `gs->paused` must remain
  checkpoint-owned. A failed paused-resume smoke proved that writing replay pause
  into `gs->paused` can preserve the visual pause while desyncing after resume.
- BAR's replay pause button now uses `Spring.SetReplayPaused` through the same
  Lua helper as paused timeline catch-up, with the old pause command only as a
  fallback. Manual pause-then-jump should no longer exercise a different pause
  ownership path than the smoke-tested UI jump seam.
- `ReplayTimelineSelfTestPauseViaButton=1` gives that helper an automated smoke
  seam. Evidence in
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_pause_button_seam_20260611/write/infolog.txt`
  logs `pause-api source=manual-button paused=1 accepted=1`, then API checkpoint
  load, exact paused target reach, and resume from the same frame.
- Full sync-backed evidence for the same path is
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_pause_button_exact_sync_20260611/write/infolog.txt`:
  manual-button pause API accepted, target `1200` resolved to checkpoint `1170`,
  bounded paused catch-up `30` frames, exact paused target/resume at `1200`, and
  digest `oh6rmUltongIJMqlhqyPRg==` for frames `1170..2999`.
- Current acceptance evidence:
  `.cache/replay-checkpoint-smoke/run_20260611_002746/write/infolog.txt`
  restored request `30`, target `1200`, checkpoint `1170`, `paused=1`, resumed
  to `1501`, and matched post-restore digest
  `WhPFMVwjoF/c0wdwHRYjHw==` for `1170..1499`.
- Unpaused acceptance evidence:
  `.cache/replay-checkpoint-smoke/run_20260611_002746_unpaused/write/infolog.txt`
  restored request `37`, target `1200`, checkpoint `1170`, `paused=0`, resumed
  to `1501`, and matched the same post-restore digest for `1170..1499`.
  This required the self-test to avoid `pause 0` in the unpaused path because
  hosted-demo replay treats that command as a toggle.
- BAR LuaUI now has a gated unsynced restore transition controlled by
  `ReplayCheckpointTransition`. It is useful for the demo branch, disabled in
  smoke configs, and should be kept separate from any upstream correctness PR
  unless BAR wants the UX polish too. The preload delay knobs are opt-in and
  default to `0`; the tested preload path advanced the source replay frame
  before dispatch, so it is not ready as default behavior.
- The manual timeline demo launcher is now demo-branch convenience rather than
  upstream correctness surface: with `-SpringExe` it writes a unique evidence
  dir, forces checkpoint timeline jumps, enables the transition unless
  `-NoTransition`, disables transition preload delay, resets stale
  transition/pause-catchup state, enables visual cleanup windows, waits for the
  engine process, then prints a post-run marker summary. `-RequireCheckpointJump`,
  `-RequireCheckpointApi`, and `-RequireCheckpointMarkers` are useful for
  hand-demo evidence but should stay out of a minimal upstream correctness PR.
- Transition evidence is now automated rather than manual-only:
  `.cache/replay-checkpoint-smoke/run_20260610_223907_transition_default_hold_20260611/write/infolog.txt`
  required `[ReplayCheckpointTransition] start`, captured deferred
  `restored`/`resume-start` screenshots with the visible time-machine overlay at
  restored frame `180`, logged `resume-deferred` before unpausing, restored
  `390 -> 180`, skipped `1500` decals, resumed to `600`, and had no
  desync/checksum-mismatch patterns.
- The BAR timeline smoke can also opt into transition rendering with
  `-EnableTransition`. Evidence in
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_transition_visual_api_20260611/write/infolog.txt`
  and its screenshots proves the timeline self-test path starts the overlay for
  target `180`, uses `Spring.LoadReplayCheckpoint`, keeps GUI Shader `rects=0`
  at reached/post-target, and leaves the raised timeline visible.
- BAR timeline self-test now has a checkpoint-owned mode. The default smoke
  path asserts `checkpoint-request`, rejects `skip-request`, checks the resolved
  checkpoint frame, and compares post-restore SYNCCHECK segments. Current
  post-refactor evidence covers forward `30 -> 1200` via checkpoint `1170`
  and backward `390 -> 180` via checkpoint `180`.
- The timeline self-test also has a paused UI-jump mode:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_paused_restore_serialguard_20260611/write/infolog.txt`
  shows `390 -> 180`, checkpoint `180`, engine restore `paused=1`, and timeline
  reached `paused=1`. Treat this as pause-state evidence for the BAR UI path;
  the long forward/backward SYNCCHECK timeline smokes remain the correctness
  authority.
- The unpaused counterpart
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_unpaused_state_summary_20260611/write/infolog.txt`
  shows `30 -> 1200`, checkpoint `1170`, bounded catch-up `30`, engine restore
  `paused=0`, timeline reached `paused=0`, no legacy skip, and GUI Shader
  `rects=0` at the reached screenshot. This closes the marker-level pause-state
  proof for both paused and unpaused BAR timeline jumps.
- The stronger paused-forward proof is now
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_paused_resume_precise_catchup_sync_20260611/write/infolog.txt`.
  It uses `Spring.LoadReplayCheckpoint(1200)`, resolves checkpoint `1170`,
  catches up exactly to frame `1200` while preserving paused state, resumes from
  `1200`, and matches digest `oh6rmUltongIJMqlhqyPRg==` for `1170..2999`.
  The earlier synced-pause attempt failed after resume with first desync at
  frame `1260`, so this should stay an explicit regression case.
- The smoke harness now enforces exact target frames for paused timeline
  reached/catch-up/resume markers. Current strict evidence:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_paused_resume_exact_assert_3001_20260611/write/infolog.txt`
  (`1170 -> 1200`, digest `oh6rmUltongIJMqlhqyPRg==`) and
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_paused_resume_exact_assert_20260611/write/infolog.txt`
  (`180 -> 180`, digest `YYyR2AD+hpZsXxod+hQ4nA==`). Use `QuitFrame 3001`
  on the 3000-frame fixture when `RequireSynctestMarkers` is enabled; quitting
  at `3000` can exit before the end marker is logged.
- Demo polish remains separate from sync correctness. The visual restore sheets
  and imploding effect artifacts reported from `run_20260610_223907` should be
  treated as blocking UI/demo regressions even when SYNCCHECK is green.
- The first visual-cleanup seam keeps ownership local. Recoil now emits only a
  restore cleanup marker (`ReplayCheckpointRestoreSerial`, restore/request
  frames, and cleanup-until frame); BAR widgets decide which unsynced caches to
  discard.
- Current BAR-side cleanup skips saved `DecalsGL4` restore during the cleanup
  window, clears `Unit Repeat Icons` VBO state for a new restore serial, and
  refreshes those icons after cleanup, and guards `Ecostats` render-to-texture
  rectangles. Latest unpaused smoke kept the digest green and removed the
  previous stale `UnitID` and nil-rect widget failures.
- BAR `GUI Shader` cleanup is now explicitly demo-branch visual polish: it
  suppresses and clears blur registrations for
  `ReplayCheckpointGuishaderCleanupFrames` after restore so stale minimap and
  player-list backdrop rectangles do not survive LuaUI reload churn. Keep this
  with BAR LuaUI/demo polish unless upstream wants the same unsynced cleanup
  behavior.
- BAR range-ring GL4 cleanup is also demo-branch visual polish. The real owner
  of the remaining magenta/orange/green sheet artifacts was not the sensor
  range widgets alone; `gui_defenserange_gl4.lua` and
  `gui_attackrange_gl4.lua` were rebuilding large stencil-filled range VBOs
  during LuaUI restore churn. They now clear/suppress range state for
  `ReplayCheckpointRangeCleanupFrames` after restore and rebuild from normal
  visible-unit/selection seams after the window closes.
- Screenshot smoke on `run_20260610_223907` now covers the reported
  magenta/green sheet regression: old sampled screenshots reached
  `15.015%` magenta and `24.442%` green coverage, while the full-hash
  restored/resumed screenshots maxed at `0.008%` magenta and `0.103%` green.
  Transition overlay captures maxed at `0.000%` magenta and `0.030%` green.
  The same run matched digest `YYyR2AD+hpZsXxod+hQ4nA==` for frames
  `180..2999`; keep screenshots as demo evidence, not a substitute for the
  SYNCCHECK invariant.
- The short visual verifier
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_visual_guishader_check_20260611/write/infolog.txt`
  now requires the GUI Shader cleanup marker and demonstrates that checkpoint
  timeline screenshot captures keep the timeline visible without stale blur
  rectangles. It is useful demo evidence, not sync authority.
- The full BAR timeline visual+SYNCCHECK smoke
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_visual_sync_20260611/write/infolog.txt`
  proves the UI path restored frame `390 -> 180`, used checkpoint `180`, caught
  up `0` frames, made no legacy skip request, cleaned GUI Shader state through
  frame `270`, and matched digest `YYyR2AD+hpZsXxod+hQ4nA==` for
  `180..2999`. Its immediate `reached` screenshot is clean and keeps the
  timeline visible.
- The separate post-battle GUI Shader blur was attributed with
  `ReplayCheckpointGuishaderDebug`: `timeline_guishader_debug_20260611` showed
  `rects=1:awards` at `timeline-post-target`. This is BAR `gui_awards.lua`
  endgame UI, not Recoil restore state. `ReplayTimelineSuppressEndAwards=1`
  now suppresses awards during replay timeline screenshot smoke; follow-up
  `timeline_awards_suppressed_20260611` showed `rects=0` at post-target and
  removed the big center blur.
- The combined follow-up
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_visual_sync_after_awards_20260611/write/infolog.txt`
  proves the backward BAR timeline path with visual screenshots and SYNCCHECK in
  one run: checkpoint request `390 -> 180`, checkpoint `180`, `0` catch-up, no
  legacy skip, no post-target `awards` rect, and digest
  `YYyR2AD+hpZsXxod+hQ4nA==` for frames `180..2999`.
- The matching forward evidence
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_visual_sync_after_awards_20260611/write/infolog.txt`
  proves the BAR timeline forward path uses checkpoint restore instead of
  direct skip: request `30 -> 1200`, checkpoint `1170`, `30` bounded catch-up
  frames, no legacy skip, no post-target `awards` rect, and digest
  `oh6rmUltongIJMqlhqyPRg==` for frames `1170..2999`.
- The API-gated follow-ups prove BAR no longer needs the command seam for the
  automated UI path. Forward evidence in
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_forward_visual_sync_api_20260611/write/infolog.txt`
  used `api-load target=1200 accepted=1`, no command fallback, checkpoint
  `1170`, `30` catch-up, and digest `oh6rmUltongIJMqlhqyPRg==` for
  `1170..2999`. Backward evidence in
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_backward_visual_sync_api_20260611/write/infolog.txt`
  used `api-load target=180 accepted=1`, no command fallback, checkpoint `180`,
  `0` catch-up, and digest `YYyR2AD+hpZsXxod+hQ4nA==` for `180..2999`.
- Timeline ergonomics were adjusted in `gui_replaybuttons.lua` from
  `y=0.03,h=0.024` to `y=0.045,h=0.030`, moving the bar farther from the screen
  edge and increasing click height. Short screenshot evidence:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_ergonomics_after_raise_20260611/write/infolog.txt`
  used `api-load target=180 accepted=1`, checkpoint `180`, no command fallback,
  and saved reached/post-target screenshots with the raised timeline visible.
- The manual demo launcher evidence gate now reports `guishader_cleanups` and
  fails on known LuaUI restore-error patterns. It now also reports checkpoint
  marker loads, API/command checkpoint load transport, and paused-catchup
  failures; `-RequireCheckpointApi` and `-RequireCheckpointMarkers` make those
  checks fail-fast for hand-demo evidence. Current older evidence in
  `.cache/replay-timeline-synctest/run_20260610_223907/replay_timeline_demo_write_demo_launcher_evidence_parser_20260611/infolog.txt`
  shows one checkpoint request, one transition start, one GUI Shader cleanup,
  no legacy skip, no desync/checksum mismatch, and no LuaUI error patterns.
- Smoke verifier hardening now matches the goal wording more closely: checkpoint
  and timeline smokes reject explicit desync warnings, sync-hash/replay/checksum
  mismatches, keyframe differences, and LuaRules/LuaUI restore call-in errors.
  Timeline smoke also rejects checkpoint resolutions after the requested frame,
  making "nearest checkpoint at or before target" an explicit harness invariant.
  This should stay with the first fixture/harness extraction.
- Hardened verifier rerun on the visual-regression fixture:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_api_hardened_checkpoint_gate_20260611/write/infolog.txt`.
  Command shape: target `180` from start `390`, `-RequireCheckpointRestore`,
  `-RequireCheckpointApi`, `-EnableTransition`, and `-CaptureScreenshots`.
  The run used `api-load target=180 accepted=1`, no command fallback, resolved
  target `180` to checkpoint `180`, catch-up span `0`, GUI Shader `rects=0` at
  reached/post-target, skipped `1500` stale decals, and saved three timeline
  screenshots with the transition overlay and raised timeline visible. Treat it
  as API/harness/visual evidence only: the old fixture still logs the known
  archive-copy warning and the run did not request SYNCCHECK hash authority.
- Range-ring visual cleanup evidence on the same old visual fixture:
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_visual_cleanup_range_owner_20260611/write/infolog.txt`
  proved the restore window no longer shows the broken range sheets, and
  `.cache/replay-timeline-smoke/run_20260610_223907_timeline_visual_cleanup_range_rebuild_20260611/write/infolog.txt`
  proved the widgets rebuild cleanly after the `180..480` cleanup window. These
  are visual/API runs only; keep SYNCCHECK equivalence as the merge authority.
- Fresh current-worktree fixture:
  `.cache/replay-timeline-synctest/run_20260611_155032`, duration `1500`,
  seed `1776250575`, recorded digest `1ElWoXMT4nW/SjOg0lZhCA==` for
  `0..1499`, and checkpoint bundle
  `write_episode_0/demos/rcp_6111dfdf.replay-checkpoints` with `19` save files.
  Marker scan found no archive-copy warning in the recording log.
- Clean forward API/SYNCCHECK authority on that fresh fixture:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_api_sync_20260611/write/infolog.txt`.
  It used `api-load target=1200 accepted=1`, no command fallback, no
  `skip-request`, resolved target `1200` to checkpoint `1170`, caught up `30`
  frames, reached exact `current=1200 target=1200 paused=0`, and matched
  restored digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for frames `1170..1499` against
  the recorded replay. This is the clean current-worktree forward timeline
  sync-authority evidence for the hardened checkpoint resolver gate.
- Clean backward API/SYNCCHECK authority on the same fresh fixture:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_fresh_api_sync_20260611/write/infolog.txt`.
  It used `api-load target=180 accepted=1`, no command fallback, no
  `skip-request`, resolved target `180` to checkpoint `180`, catch-up span `0`,
  reached the target path, and matched restored digest
  `uwYRIjtUtKJgWnHDj29Piw==` for frames `180..1499`.
- Clean paused/manual-button API/SYNCCHECK authority on the same fresh fixture:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_pause_button_sync_20260611/write/infolog.txt`.
  The self-test used `pause-api source=manual-button paused=1 accepted=1`,
  resolved `1200 -> 1170`, prepared bounded catch-up over `30` frames, unpaused
  only for catch-up, re-paused at `current=1200 target=1200 paused=1`, then
  resumed only for the post-target hash window. Digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` matched frames `1170..1499`.
- Fresh post-range-cleanup visual+SYNCCHECK authority:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_visual_sync_after_range_cleanup_20260611/write/infolog.txt`
  proves forward `30 -> 1200` with API load, checkpoint `1170`, bounded
  catch-up `30`, clean screenshots, range rebuild after the cleanup window, and
  digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`.
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_fresh_visual_sync_after_range_cleanup_20260611/write/infolog.txt`
  proves backward `1200 -> 180` with API load, checkpoint `180`, catch-up `0`,
  clean screenshots, range rebuild after the cleanup window, and digest
  `uwYRIjtUtKJgWnHDj29Piw==` for `180..1499`. Both reject command fallback and
  legacy skip.
- Fresh paused forward visual+SYNCCHECK after range cleanup:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_pause_visual_sync_after_range_cleanup_20260611/write/infolog.txt`
  proves the manual pause-button path with expected restored pause state `1`.
  It used API load, checkpoint `1170`, bounded catch-up `30`, exact reached
  `current=1200 target=1200 paused=1`, clean screenshots, range rebuild after
  the cleanup window, and digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`.
  The run rejected command fallback and legacy skip.
- Fresh paused backward visual+SYNCCHECK:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_fresh_pause_visual_sync_20260611/write/infolog.txt`
  proves the manual pause-button path in the reverse direction. It used API
  load, checkpoint `180`, catch-up `0`, exact reached
  `current=180 target=180 paused=1`, exact resume from frame `180`, range
  rebuild after the cleanup window, and digest `uwYRIjtUtKJgWnHDj29Piw==` for
  `180..1499`. The reached screenshot keeps the raised timeline visible and
  GUI Shader reports `rects=0`, while rejecting command fallback and legacy
  skip.
- Fresh transition v3 visual+SYNCCHECK after range cleanup:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_transition_v3_visual_sync_20260611/write/infolog.txt`
  proves the gated transition on the clean fixture. It logged transition start
  `target=1200 request=390`, transition draw at frame `1170`, API load,
  checkpoint `1170`, bounded catch-up `30`, exact reached
  `current=1200 target=1200 paused=0`, clean screenshots, range rebuild after
  the cleanup window, and digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`. The reached screenshot shows
  `TIME MACHINE ONLINE`, radial rings/spokes, and `FORWARD +810f` with the
  raised timeline visible, while still rejecting command fallback and legacy
  skip.
- Transition text now has a `gl.Text` path with inline color codes plus the
  existing font-wrapper fallback. Fresh sync-backed visual evidence:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_transition_gltext_color_sync_20260611/write/infolog.txt`.
  It required API load, checkpoint markers, transition draw, screenshots,
  SYNCCHECK markers, and sync hash; restored `390 -> 1200` through checkpoint
  `1170`, caught up `30`, matched digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`, and saved readable transition
  screenshot `screen_2026-06-11_19-00-44-870.png`.
- Checkpoint availability now has a BAR-facing API seam:
  `Spring.GetReplayCheckpoints()` enumerates sorted bundle frames through
  `ReplayCheckpointHandler::GetAvailableCheckpointFrames()`, and BAR draws
  timeline checkpoint tick marks from that list. Fresh marker evidence in
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_checkpoint_markers_visual_sync_20260611/write/infolog.txt`
  required the `[ReplayTimelineCheckpoints] loaded count=` marker, used API
  load for `1200`, restored checkpoint `1170`, caught up `30` frames, reached
  `1200`, and matched digest `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`.
  The reached screenshot shows visible checkpoint ticks on the raised timeline.
  Upstream review should still decide whether the read-only listing API belongs
  in its current unsynced-control location or a narrower replay/read surface.
- The paused timeline catch-up handoff now has an explicit failure seam instead
  of CPU-time settle fallbacks. BAR waits for observed replay speed/pause state
  with configurable update-count budgets and logs
  `[ReplayTimelinePausedCatchup] failed` on timeout; the timeline smoke rejects
  that marker. Fresh evidence:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_fresh_pause_settle_checks_sync_20260611/write/infolog.txt`
  used manual-button pause, API load, checkpoint `1170`, bounded catch-up `30`,
  exact paused target/resume at `1200`, clean screenshots, and digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`.
- The manual demo launcher now has a fresh strict gate on the same fixture:
  `.cache/replay-timeline-synctest/run_20260611_155032/replay_timeline_demo_write_demo_launcher_strict_api_markers_20260611/infolog.txt`.
  It ran with `-RequireCheckpointJump`, `-RequireCheckpointApi`, and
  `-RequireCheckpointMarkers`, restored target `1200` through checkpoint `1170`,
  loaded `19` checkpoint tick frames, used `api_loads=1`, `command_loads=0`,
  had `skip_requests=0`, `paused_catchup_failures=0`, no desync/checksum or
  LuaUI error patterns, and `max_catchup_span=30`. Keep this as launcher
  evidence; physical click feel and transition timing still need a manual demo
  pass.
- The launcher also has a screenshot evidence gate via `-CaptureScreenshots`.
  Fresh run
  `.cache/replay-timeline-synctest/run_20260611_155032/replay_timeline_demo_write_demo_launcher_screenshot_gate_gltext_color_20260611/infolog.txt`
  required screenshot markers, a deferred capture, GUI Shader reached debug
  dump, checkpoint API load, and marker loading; it saved two PNGs and rejected
  command fallback, legacy skip, paused catch-up failures, desync/checksum, and
  LuaUI restore-error patterns.
- The timeline smoke has an opt-in widget-click seam:
  `-ViaTimelineClick`/`ReplayTimelineSelfTestViaTimelineClick=1` computes a
  timeline click point, calls `widget:MousePress`, and verifies
  `frame_from_timeline_x` maps to the target before checkpoint load. Fresh
  evidence in
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_forward_click_path_visual_sync_20260611/write/infolog.txt`
  logged `timeline-click current=390 target=1200 click_frame=1200 ... active=1`,
  used API load through checkpoint `1170`, bounded catch-up `30`, reached
  `1200` unpaused, saved clean transition/tick screenshots, and matched digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`. This narrows the manual-demo gap,
  but physical click feel still needs a real pass.
- The same widget-click seam now covers the automated forward/backward and
  paused/unpaused matrix. Paused forward
  `timeline_forward_click_path_paused_visual_sync_20260611` used the
  pause-button helper, clicked `390 -> 1200`, restored checkpoint `1170`,
  caught up `30`, reached exact `1200` with `paused=1`, resumed from `1200`,
  saved clean transition/tick screenshots, and matched digest
  `rJ3pD3afQ5t3xjHCSl7AwQ==` for `1170..1499`.
  Unpaused `timeline_backward_click_path_visual_sync_20260611` restored
  `1200 -> 180`, preserved `paused=0`, saved clean rewind screenshots, and
  matched digest `uwYRIjtUtKJgWnHDj29Piw==` for `180..1499`. The paused variant
  initially found a self-test bug: backward jumps could mark reached before the
  pending click dispatched, causing an early resume and restore `paused=0`.
  `gui_replaybuttons.lua` now blocks reached markers while a paused jump is
  pending; `timeline_backward_click_path_paused_visual_sync_fixed_20260611`
  proves exact `reached current=180 target=180 paused=1`, then resumes from
  `180` and matches the same digest.
- BAR visual cleanup now guards against over-broad cleanup and missing rebuilds.
  Sensor range cleanup is active only between the restored frame and cleanup
  deadline, queues a visible-unit refresh while stale state is suppressed, and
  rebuilds after the window; launchers reset restore/cleanup config markers.
  Fresh evidence:
  `.cache/replay-timeline-smoke/run_20260611_155032_timeline_backward_click_visual_cleanup_refresh_rebuild_20260611/write/infolog.txt`
  logged stale visual cleanup at frame `180`, attack/defense/sensor rebuild at
  frame `482`, clean screenshots, API checkpoint load, no legacy skip/command
  fallback, and digest `uwYRIjtUtKJgWnHDj29Piw==` for `180..1499`.

## 2026-06-09 PR Extraction Notes

- Keep first extraction around reproducible fixtures and smoke scripts. They
  should prove restore/resume by sync equivalence and reject visual-only success.
- Separate temporary diagnostics from correctness code. Current diagnostics are
  named and gated by `ReplayCheckpointDebugSignatureFrame`,
  `ReplayCheckpointDebugDumpFrame`, and `ReplayCheckpointDebugSyncTrace*`; keep
  them behind those gates or move them into test-only/debug-only patches.
- Candidate correctness chunks: QTPFS checkpoint path snapshots, QTPFS
  node-layer rebuild-on-load, serialized quadfield restore, smooth-mesh
  checkpoint-load queue preservation, sync-check checksum boundary restore, and
  replay frame/pause/demo-reader accounting.
- Candidate future seams: CREG round trips for each restored owner, a node-layer
  checksum/signature test after map/blocking restore, a QTPFS one-update
  determinism test, a smooth-mesh load/postload queue-preservation test, and an
  end-to-end SYNCCHECK replay restore/resume test.
