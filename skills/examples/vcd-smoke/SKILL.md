---
name: wavegrep-vcd-smoke
description: >
  Run the bundled VCD fixture through every wfm_query command and confirm the
  CONTRACT.md invariants hold. Use to sanity-check an install, demo the tool, or
  as a template for a house acceptance check on a real waveform.
---

# wavegrep-vcd-smoke (example subskill)

Built on the `wavegrep` base skill. This is both an **example** and an
**executable spec**: it exercises the whole command surface against a known
input and checks the guarantees an agent relies on.

Target: `skills/examples/fixtures/counter.vcd` (see that directory's README for
what the signals do). Swap in a real `.vcd` to turn this into an acceptance
check for your own dump — the expected *values* change, the *invariants* do not.

## Steps

Run each and check the invariant. Stop at the first failure and report it.

| # | Command | Expected (fixture) | Invariant (any VCD) |
|---|---|---|---|
| 1 | `info` | `Signals: 8`, `Scopes: 2`, `Time range: 0.000000 ns - 205.000 ns` | exit 0; has `Signals:` / `Scopes:` / `Time range:` lines |
| 2 | `scopes dut` | lists `tb.dut`; `Total: 1 scopes` | exit 0; ends `Total: N scopes` |
| 3 | `signals count` | `wire  tb.dut.count`; `Total matches: 1` | exit 0; ends `Total matches: N` |
| 4 | `value tb.dut.count 60` | `Value:  00000100 (0x04)` | exit 0; stderr has `Resolved: <path>`; value is `<bin> (0x<hex>)` |
| 5 | `value /tb/dut/count 95` | `Resolved: tb.dut.count`; `0x08` | slash path normalises to the same signal |
| 6 | `changes tb.dut.count 40 120` | 8 rows; `Total: 8 value changes` | exit 0; `Time / Binary / Hex` header; times are decimal ns |
| 7 | `freq dut` | `clk  100.0 MHz  (period 10.000 ns)` | exit 0; `Block: <scope>` then one line per clock |
| 8 | `value bogus_signal 10` | — | **exit 1** (not 0); no fabricated value |
| 9 | `value clk 10` | — | **exit 1**; stderr lists `tb.clk` and `tb.dut.clk`; never auto-picks |
| 10 | `value` (no args) | — | **exit 2** (usage) |
| 11 | `debug dut 50 -w 20` | snapshot table + `--- transitions in window ---` | exit 0 |
| 12 | `printf 'info\nfreq dut\n' \| wfm_query <wf> batch` | `>>> info` / `>>> freq dut` blocks | exit 0; each line echoed as `>>> <line>` |

## Rules

- Steps 8–10 are the important ones: a non-zero exit must **never** be followed
  by a reported result (guardrails **G1**, **G3**). If step 9 returns a value
  instead of the candidate list, that is a contract violation — report it.
- Use the `Resolved:` line verbatim; do not convert `.`/`/`.
- This mirrors `tests/test_contract.py`. If they disagree, the test is the
  source of truth.
