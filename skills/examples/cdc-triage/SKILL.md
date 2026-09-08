---
name: wavegrep-cdc-triage
description: >
  Given a failure time and two blocks on different clocks, dump the activity in
  both clock domains around that time so a clock-domain-crossing bug can be
  reasoned about. Assumes the wavegrep base skill.
---

# wavegrep-cdc-triage (example subskill)

Built on the `wavegrep` base skill. A starting point for triaging a suspected
clock-domain-crossing (CDC) issue: line up what each side of the crossing was
doing at the failure time. Replace the `CONFIG` placeholders with your design's
names.

## CONFIG — edit this

| Key | Placeholder | Meaning |
|---|---|---|
| `TX_BLOCK` | `core_top` | Block on the source clock |
| `RX_BLOCK` | `uart` | Block on the destination clock |
| `HANDSHAKE` | `*req*`, `*ack*`, `*valid*`, `*ready*` | Crossing signals of interest |
| `WINDOW_NS` | `50` | Half-window around the failure time |

## Procedure

1. **Resolve both blocks:** `wfm_query <wf> scopes <TX_BLOCK>` and
   `... scopes <RX_BLOCK>`. Stop and report if either is missing (**G3**).
2. **Measure both clocks:** `wfm_query <wf> freq <TX_BLOCK>` and
   `... freq <RX_BLOCK>`. Record both periods; note the ratio — a
   non-integer ratio is the whole reason CDC synchronisers exist.
3. **Snapshot each domain at the failure time `T`:**
   - `wfm_query <wf> debug <TX_BLOCK> <T> -w <WINDOW_NS>`
   - `wfm_query <wf> debug <RX_BLOCK> <T> -w <WINDOW_NS>`
   `debug` already filters to handshake-class signals, so this is usually
   enough. On SHM the extra signals are nearly free — take the wider snapshot.
4. **Follow specific crossing signals** if `debug` points at one:
   `wfm_query <wf> changes '<TX_BLOCK> <sig>' <T-WINDOW_NS> <T+WINDOW_NS>` and
   the matching `RX_BLOCK` signal. Align the two transition lists by time.
5. **Report** for each side: clock period, the last stable value before `T`,
   any transition inside the window, and whether an `RX` sample lands within one
   `RX` clock of an `TX` change (classic metastability window).

## Rules

- Always pass a time window to `changes` here — a fast clock can have millions of
  transitions over a run (base-skill guidance).
- Quote `Resolved:` paths verbatim.
- If a clock's `freq` cannot be measured (block idle in the window), say so;
  do not assume a nominal frequency (**G1**).
