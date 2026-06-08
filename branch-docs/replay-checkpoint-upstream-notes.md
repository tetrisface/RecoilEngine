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
  `docs/replay-checkpoint-regression-tests.md`, including the subsystem, observed
  divergence, attempted fix, and eventual test seam.
- Incremental upstream contributions are likely easiest to review when they are
  independent of the full replay-bundle experiment: missing CREG fields,
  post-load invariants, pathing restore fixes, frame-accounting cleanup, and
  deterministic smoke fixtures.
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

