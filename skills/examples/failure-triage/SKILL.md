---
name: wavegrep-failure-triage
description: >
  House runbook for first-pass triage of a simulation failure from its
  waveform: orient, locate the failing block, snapshot it, and walk back the
  driving signals. Assumes the wavegrep base skill.
---

# wavegrep-failure-triage (example subskill)

Built on the `wavegrep` base skill. This is a **template runbook** — a fixed
order of `wfm_query` calls so triage is consistent no matter who (or which
agent) runs it. Adapt the steps and thresholds to your team.

Inputs the user provides: the waveform path, the failure time `T` (from
`sim.log`, in ns), and ideally the block or signal named in the error.

## Runbook

1. **Orient.** `wfm_query <wf> info` — confirm the dump covers `T` and has the
   scale/format you expect.
2. **Locate the block.**
   - If the user named a signal or full path → skip to step 3 with it.
   - If they named a block → `wfm_query <wf> scopes <block>` to get the real
     path. More than one match: present the list, ask, do not pick (**base
     skill**).
   - Nothing named → `wfm_query <wf> scopes <keyword-from-error-text>`.
3. **Clock context.** `wfm_query <wf> freq <block>` — get the period so every
   later time can be quoted in cycles as well as ns.
4. **Snapshot at the failure.** `wfm_query <wf> debug <block> <T> -w 50`.
   This is the highest-value call: it returns the handshake/valid/ready/err
   signals in the block subtree around `T` for roughly the cost of one `value`.
5. **Walk back.** Pick the signal in the snapshot that is in the wrong state
   (an `err`/`*_fault` asserted, a `ready` stuck low, a `valid` with no
   consumer). For each:
   `wfm_query <wf> changes '<block> <sig>' <T-500> <T+100>` — find the last
   transition before `T` and what changed with it.
6. **Cross-check any suspicious `value`.** Guardrail **G2**: if you read a
   `value` on a signal that looks long-idle (config/mode/strap), also run
   `changes '<block> <sig>' <T-100000> <T>` and confirm consistency before you
   rely on it.
7. **Report.** Failing block (resolved path), clock period, the snapshot table,
   the earliest signal you can show was already wrong and its last transition
   time (ns + cycles), and a one-line hypothesis. Mark anything unverified as
   unverified.

## Optional enforcement wrapper — G2

`wrap_value.sh` (sketch — drop next to this skill and point the agent at it
instead of raw `value` for step 6):

```bash
#!/usr/bin/env bash
# usage: wrap_value.sh <wf> <signal> <time_ns>
set -euo pipefail
wf=$1 sig=$2 t=$3
v=$(wfm_query "$wf" value "$sig" "$t") || { echo "value failed" >&2; exit 1; }
lo=$(awk -v t="$t" 'BEGIN{print t-100000}')
c=$(wfm_query "$wf" changes "$sig" "$lo" "$t" -n 5 || true)
echo "$v"
echo "--- G2 cross-check (changes $lo..$t) ---"
echo "$c"
```

The point is not this exact script — it is that the cross-check runs every time,
not only when someone remembers.

## Rules

- Fixed order. Do not skip `info`/`freq` "to save a call" — they are cheap and
  they catch wrong-dump / wrong-time mistakes early.
- `Resolved:` paths verbatim in the report.
- Non-zero exit anywhere → report what failed at that step; never fill the gap
  with a plausible value (**G1**).
- The output is a hypothesis and evidence, nothing else. Do not edit RTL, the
  testbench, or any source, and do not re-run the sim to "confirm" — that is a
  separate request the user makes explicitly (**G4**).
