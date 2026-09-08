---
name: wavegrep-reset-check
description: >
  Check that reset deasserts cleanly across a named set of blocks in a waveform.
  Assumes the wavegrep base skill. Use when asked "did reset come out clean",
  "is <block> out of reset at time T", or during power-on / bring-up debug.
---

# wavegrep-reset-check (example subskill)

Built on the `wavegrep` base skill. This encodes **one team's** reset-sanity
workflow. Everything in `CONFIG` below is a placeholder — replace it with your
design's real names and times before use.

## CONFIG — edit this

| Key | Placeholder | Meaning |
|---|---|---|
| `RESET_SIGNAL` | `*_rst_n` | Active-low reset, bare name or glob |
| `BLOCKS` | `core_top`, `pipe`, `uart` | Blocks that must be out of reset |
| `RELEASE_BY_NS` | `5000` | Reset must be deasserted by this time |
| `SETTLE_NS` | `200` | Window after release to check for glitches |

## Procedure

1. **Confirm hierarchy.** For each block in `BLOCKS`, run
   `wfm_query <wf> scopes <block>`. If any block does not resolve, stop and
   report which — do not guess a path (guardrail **G3**).
2. **Find the reset transition.** For each block:
   `wfm_query <wf> changes '<block> <RESET_SIGNAL>' 0 <RELEASE_BY_NS + SETTLE_NS>`.
3. **Evaluate.** For each block, report:
   - the time reset deasserted (last 0→1 on an active-low reset),
   - PASS if that time ≤ `RELEASE_BY_NS`, FAIL otherwise,
   - GLITCH if there is any further transition within `SETTLE_NS` after release.
4. **Clock sanity (optional).** For any block that passed, run
   `wfm_query <wf> freq <block>` and confirm the clock is toggling after reset
   release — a block "out of reset" with a dead clock is still stuck.
5. **Summarise** as a table: block | reset-release time | verdict.

## Rules

- Use the `Resolved:` line from stderr as the signal path in your report —
  verbatim, no separator conversion (contract).
- A non-zero exit on any `changes` call → report the failure for that block, do
  not synthesise a release time (guardrail **G1**).
- Never widen the search beyond `BLOCKS` unless the user asks.
