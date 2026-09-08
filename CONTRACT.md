<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Shravya Juluru -->

# Output contract — `wfm_query` v1

This document is the **stable interface** that agents, guardrails, and subskills
build on. It is versioned. Anything described here as *guaranteed* will not
change without a major-version bump and a `CHANGELOG` entry. Anything described
as *not guaranteed* may change in a minor release — do not parse it.

Three backends implement this contract identically — `fsdb_query` (Synopsys
FSDB), `shm_query` (Cadence SST2/SHM), and `vcd_query` (IEEE 1364 VCD). That is
the point of the contract: a caller never has to know which backend ran.

`vcd_query` is the only one that runs without a vendor toolchain, so it is what
the test suite (`tests/test_contract.py`) exercises against a committed fixture
(`skills/examples/fixtures/counter.vcd`). The other two are verified by hand on
licensed hosts.

---

## Invocation

```
wfm_query <waveform> <command> [args]
```

- `<waveform>` — a `.fsdb` file, a `.shm` directory / `.trn` file, or a
  `.vcd` / `.vcd.gz` file. The dispatcher picks the backend by extension, then
  by sniffing an unknown name.
- Every `<time>` argument and every emitted timestamp is in **nanoseconds**.
- `--refresh` rebuilds the hierarchy index before querying (SHM only; accepted
  and ignored by `vcd_query`, which has no cache).

Commands: `info`, `scopes <keyword>`, `signals [pattern] [-n max]`,
`value <signal> <time_ns>`, `changes <signal> [t0_ns] [t1_ns] [-n max]`,
`freq <block>`, `debug <block> <time_ns> [-w window_ns]`, `batch [file]`.

---

## Exit codes — GUARANTEED

| Code | Meaning | What a caller must do |
|---|---|---|
| `0` | Query succeeded. `stdout` holds the result. | Trust `stdout`. |
| `1` | Query failed **or** a pattern matched more than one signal. | Do **not** treat `stdout` as an answer. On a multi-match, `stdout` is a numbered candidate list — surface it, ask which one, re-run with a full path. Never pick for the user. |
| `2` | Usage error — unknown command, missing/!parseable operands, bad flag value. | Fix the call; do not retry unchanged. |

A non-zero exit is never "probably fine." An agent that invents a signal value
after a non-zero exit has violated the contract, not the tool.

> Note: `vcd_query` and the `wfm_query` dispatcher return `2` for every usage
> error. `fsdb_query` / `shm_query` currently collapse some subcommand-level
> usage errors into `1`; treat `1` as "failed — inspect stderr" regardless, and
> only trust `stdout` on `0`.

---

## `stderr` conventions — GUARANTEED

- On a successful signal lookup (`value`, `changes`), `stderr` contains exactly
  one line:
  ```
  Resolved: <full_native_path>
  ```
  This is the ground-truth path the tool actually read. Quote **this** back to
  the user, verbatim — do not convert its separators.
- Diagnostics and warnings go to `stderr`, prefixed with the tool name
  (`wfm_query: ...`, `shm_query: ...`).
- `stdout` never carries diagnostics. `stdout` on exit `0` is result data only.

---

## Value formatting — GUARANTEED

- Signal values print as `<binary> (0x<hex>)` — e.g. `1010 (0xa)`.
- Timestamps print in **decimal nanoseconds**: 3 fractional places at or above
  1 ns, 6 fractional places below 1 ns.
- `changes`, `freq`, `scopes`, `signals` emit a header row and a trailing total,
  identical across all three backends.
- Input paths may be `/`-separated or `.`-separated; both are accepted and
  normalized. **Output** always uses the database's native separator.

---

## NOT guaranteed — do not parse

- Exact wording of human-readable prose lines, warnings, or the multi-match
  list preamble.
- Field order within `info` (the two backends expose different metadata — see
  README "Backend consistency").
- Column widths / whitespace alignment. Split on runs of whitespace, never on
  fixed columns.
- Which instance `freq` / `debug` selects when a keyword matches several scopes
  at the same depth (tie-break is traversal order — see README). Pass a longer
  path fragment if it matters.
- The hex column for real-valued (`r`) VCD signals — `vcd_query` prints the
  recorded number in both columns; only the left value is meaningful.

---

## Determinism

Given the same waveform file and the same arguments, `wfm_query` returns the
same bytes on `stdout` and the same exit code every time. It performs **no**
writes to the design, the waveform, or any shared state. The only side effect
is the per-user hierarchy-index cache (SHM), which is derived data keyed on the
`.trn` mtime + size and is safe to delete at any time. `vcd_query` keeps no
cache in this release — it re-parses the file per invocation.

This is why the tool is safe to put behind an autonomous agent: it cannot
fabricate a value, cannot mutate anything, and reports failure honestly.

The tool cannot touch your RTL because it has no code path that writes there.
Keeping the *agent* out of the edit path while it uses the tool is a separate
rule — guardrail **G4** in [`skills/GUARDRAILS.md`](skills/GUARDRAILS.md).

---

## Versioning

`wfm_query --version` prints `wfm_query <major>.<minor>` (currently
`wfm_query 1.0`). Guardrails and subskills that depend on this contract should
check the **major** version; a minor bump only adds, it never breaks what is
documented here as guaranteed.
