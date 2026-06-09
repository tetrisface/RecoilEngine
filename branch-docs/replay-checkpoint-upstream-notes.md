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
- Clean LuaRules checkpoint restore still fails acceptance:
  - target `90` restores SYNCCHECK boundary `653aa44d` but desyncs at frame
    `420`; post-restore digest `38sMPRU8iyZqbVrwYRrtDg==` over `90..419`
    first mismatches the recording at frame `391`.
  - target `180` restores SYNCCHECK boundary `50eefdc7` but desyncs at frames
    `240`, `300`, `360`, and `420`; post-restore digest
    `r7tmLxOACCQLllB5bM42aQ==` over `180..419` first mismatches at frame `222`.
- Treat the smooth-mesh guard as a necessary fix, not proof of completion. The
  next upstream-shareable seam should localize the clean-fixture first mismatch
  frames with subsystem signatures before extracting PR-ready chunks.
- This suggests another separable review chunk: make serialized smooth-mesh
  restore a local invariant, with a small regression test that proves read-map
  postload cannot replace serialized smooth queues during replay checkpoint
  restore.

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
