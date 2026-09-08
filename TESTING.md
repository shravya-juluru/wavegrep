<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Shravya Juluru -->

# Testing

## What's covered where

| Backend | How it's verified | Status |
|---|---|---|
| VCD (`vcd_query`) | `tests/test_contract.py` + a committed fixture, in CI on every push | automated |
| SST2 / SHM (`shm_query`) | by hand on a licensed Xcelium host | needs more environments |
| FSDB (`fsdb_query`) | by hand on a licensed Verdi host | **unverified this cycle — not yet compiled** |

The VCD path proves the [output contract](CONTRACT.md). The two vendor backends
implement the *same* contract but can't run in CI (Verdi is needed to build the
FSDB backend; Xcelium to run the SHM one), so they need testing on real hosts —
ideally several, because the useful bugs come from **environment variety**:
different Verdi/Xcelium versions, VCS vs. Xcelium dumps, different
hierarchy conventions, and dumps far larger than any test fixture.

It is safe to run against real design data: `wfm_query` is **read-only** — it
never writes to the design or the waveform (see [CONTRACT.md](CONTRACT.md) §
Determinism). Worst case is a crash or a wrong number, not damage.

---

## Running the VCD checks (no vendor tools)

```bash
python3 tests/test_contract.py
python3 skills/examples/fixtures/gen_counter_vcd.py | diff - skills/examples/fixtures/counter.vcd
```

15 checks against `skills/examples/fixtures/counter.vcd`, plus a check that the
fixture is byte-reproducible from its generator.

---

## Validating a vendor backend on a real waveform

### 1. Build / point at the tools

- **FSDB:** `./setup.sh --verdi-home /path/to/verdi` (or `make VERDI_HOME=...`).
  Confirm you get a `fsdb_query` binary.
- **SHM:** `export WFM_XCELIUM_ROOT=/path/to/xcelium/tools`. No build.

`./wfm_query --version` should print `wfm_query 1.0`.

### 2. The principle: cross-check every answer against an oracle

A result that *looks* plausible but is wrong is the failure that matters. So for
each command, compare `wfm_query`'s output against something authoritative:

| Oracle | Use it to check |
|---|---|
| Verdi / SimVision GUI | `value` at a time, `changes` in a window, signal existence |
| `sim.log` / design spec | `freq` (clock period), time ranges |
| Your own knowledge of the design | `scopes` / `signals` counts, hierarchy paths |

### 3. Walkthrough

Pick, from a design you know: a **block name**, a **clock** whose frequency you
know, a **signal + time** whose value you can read in the viewer, and a
**signal + window** with known transitions. Then:

| # | Command | Check |
|---|---|---|
| 1 | `info` | exit 0; signal/scope counts and time range are sane for this dump |
| 2 | `scopes <block>` | your block appears; `Total: N scopes` line present |
| 3 | `signals <pattern>` | expected signals listed; `Total matches: N` present |
| 4 | `value <sig> <t>` | value **matches the viewer** at time `t`; stderr has `Resolved: <full_path>` |
| 5 | `value <sig-as-dotted-path> <t>` | a `.`-separated path resolves to the same signal as the `/` form |
| 6 | `changes <sig> <t0> <t1>` | transition times/values **match the viewer**; `Time / Binary / Hex` header; times in decimal ns |
| 7 | `freq <block>` | frequency/period **matches `sim.log`** (within rounding) |
| 8 | `debug <block> <t> -w <w>` | snapshot + `--- transitions in window ---`; the signals shown are real |
| 9 | `value <bogus_signal> <t>` | **exit 1**, no value printed (never a fabricated answer) |
| 10 | `value <ambiguous_substr> <t>` | **exit 1** + a numbered candidate list; never auto-picks |
| 11 | `value` (no operands) | **exit 2** (usage) |
| 12 | `printf 'info\nfreq <block>\n' \| wfm_query <wf> batch` | each line echoed as `>>> <line>`; exit 0 |

Steps 4, 6, 7 are the real test (does it agree with ground truth). Steps 9–11
are the safety test (does it fail honestly).

### 4. Report back

Open a **Backend validation report** issue (the form prompts for everything):
tool versions, waveform source + size, the step results, and — importantly —
**any result that disagreed with an oracle**, with the command, what
`wfm_query` said, and what the oracle said.

> **Scrub before you paste.** Replace real signal paths, block names, and
> filesystem paths with placeholders in anything that goes into a public issue.
> If a real path is essential to explain a bug, send it privately instead.
