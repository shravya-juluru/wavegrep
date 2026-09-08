---
name: wavegrep
description: >
  Query waveform files using natural language — Synopsys FSDB (.fsdb), Cadence
  SimVision SST2 (.shm), and IEEE 1364 VCD (.vcd).
  Use when the user asks about signal values, clock frequency, transitions,
  waveform debug, or anything related to waveforms. Invoke with
  /wavegrep <waveform_path> or /wavegrep and Claude will ask for the path.
---

# wavegrep skill

Natural language frontend for querying waveform databases. This is the **base
skill** — the raw command surface and safe defaults.

- The output format, exit codes, and `Resolved:` convention are a stable,
  versioned interface: see [`CONTRACT.md`](CONTRACT.md). Rely only on what it
  marks *guaranteed*.
- Team-specific workflows and rules layer on top, in [`skills/`](skills/README.md):
  worked subskills (`reset-check`, `cdc-triage`, `failure-triage`) and
  [`skills/GUARDRAILS.md`](skills/GUARDRAILS.md). If a subskill is installed for
  the task at hand, follow it; otherwise use this skill directly.
- The same commands are available to non-Claude-Code agents through the stdio
  MCP server (`mcp_server.py`).

## Usage

```
/wavegrep <waveform>
```

Always call **`wfm_query`** — it dispatches to the right backend by format, so
you never need to pick one:

| Input | Backend | Source |
|---|---|---|
| `*.fsdb` | `fsdb_query` (native C++, FsdbReader) | VCS simulation |
| `*.shm` directory or `*.trn` file | `shm_query` (Python, Cadence SST2) | Cadence SimVision |
| `*.vcd` / `*.vcd.gz` | `vcd_query` (Python, stdlib only) | Verilator / Icarus / GHDL / cocotb / EDA Playground |

All backends live in the same directory as this skill file
(`.claude/skills/wavegrep/`). If the user does not provide a path, ask for it.
The VCD backend needs no vendor tools; a runnable fixture is at
`skills/examples/fixtures/counter.vcd`.

## Command mapping

| User intent | Command |
|---|---|
| "what blocks exist", "find scope core", "list scopes" | `scopes <keyword>` |
| "frequency", "clock speed", "period", "how fast" | `freq <block>` |
| "value of X at time Y" | `value <signal> <time>` |
| "what signals", "list signals", "find signal" | `signals <pattern>` |
| "transitions", "changes", "toggling" | `changes <signal> [t0] [t1]` |
| "file info", "how many signals" | `info` |
| "debug", "what's happening at time X in block Y" | `debug <block> <time> [-w ns]` |
| multiple queries at once | `batch <file>` or pipe to stdin |

## Commands

```
wfm_query <waveform> info
wfm_query <waveform> scopes <keyword>
wfm_query <waveform> signals [pattern] [-n max]
wfm_query <waveform> value <signal> <time>
wfm_query <waveform> changes <signal> [begin] [end] [-n max]
wfm_query <waveform> freq <block_name>
wfm_query <waveform> debug <block_name> <time> [-w window_ns]
wfm_query <waveform> batch [file]
```

Identical surface for both formats. All times are in **nanoseconds** (matching
sim.log); each backend converts internally.

## Signal matching

All three backends support the same ways to specify signals:

1. **Full path**: `/top/dut/core_0/clk` — exact match, fastest (instant early exit). A dot-separated path (e.g. `top.dut.core_0.clk`, as copied from a UVM message or RTL reference) is auto-normalized and tried as an exact match first too — no manual conversion needed. This works in both directions: SST2 hierarchy is natively dot-separated, and slash paths are normalized to it.
2. **Bare name**: `sys_clk` — auto-wraps as substring match `*sys_clk*`, case-insensitive.
3. **Scoped search**: `'core clk'` (two words with space) — finds the scope matching "core", then searches only within it for "clk".
4. **Glob pattern**: `'*core*data*'` — standard shell glob, case-insensitive.

On success, stderr prints `Resolved: <full_path>` so you can verify which signal
was matched.

## SHM notes

- **The first query on a `.shm` builds a hierarchy index** (~6s for a large ~1M-signal dump) cached per-user under `$WFM_CACHE_DIR` (defaults to `$WFM_SCRATCH_ROOT/$USER/wfm_cache`, then `$TMPDIR`, then `~/.cache/wavegrep`). Subsequent queries are ~1s. Tell the user this is happening if they're waiting on a first query; do not assume the tool has hung.
- **Regenerated waveforms are handled automatically.** The cache key includes the `.trn` mtime and size, so a rebuilt dump is re-indexed and the stale index deleted — stale hierarchy can never be served. Never tell users to clear the cache by hand; `--refresh` exists for the rare case where the file didn't change but the index should be rebuilt.
- **`debug` is the best-value command on SHM.** Extra signals are nearly free — dozens of signals cost about the same ~1.2s as one — because the cost is a fixed per-invocation floor, not per-signal. Prefer one `debug <block>` over several `value` calls.
- **`changes` without a time window can return enormous output** — a single clock can have millions of transitions over a full run. Always pass `t0`/`t1` for SHM unless the user explicitly wants everything.
- **VWDB-generation databases are not supported.** If a `.shm` directory has no `.trn` inside, `shm_query` says so and names the tools that would be needed (`vwdbReport`/`vwdbExtract`). Report that rather than trying to work around it.
- **Output format is identical across backends.** Timestamps are decimal ns (3 places at or above 1 ns, 6 below), values print as `<binary> (0x<hex>)`, and `changes`/`freq`/`scopes`/`signals` use the same headers and totals as the FSDB path. Report results the same way regardless of which backend ran.
- **`debug` applies the same interesting-signal name filter** (clk / valid / ready / rdy / enable / _en / data / err / req / ack / grant / done / busy / stall), so it surfaces handshake signals rather than an arbitrary slice. On SHM the list is overridable via `WFM_DEBUG_KEYWORDS`; on FSDB it is compiled in. SHM also caps the collected set at `WFM_DEBUG_CAP` (40); FSDB does not cap it, so `debug` on a very large block prints a longer snapshot table there.
- The toolchain is resolved internally by absolute path (default `XCELIUM2409`; `XCELIUM2209` works identically) with `LD_LIBRARY_PATH` set inside the tool. Do **not** tell the user to source Cadence setup scripts or change their env. Override with `WFM_XCELIUM_ROOT` only if the default install is removed.

### Backend differences to be aware of

These are inherent to the database formats and will not be fixed:

| | FSDB | SHM (SST2) | VCD |
|---|---|---|---|
| Scope separator in output | `/top/dut/core_0/clk` | `chip_top.soc_u0...` | `tb.dut.clk` (dot) |
| `signals` type column | `wire  /path` | path only (SST2 exposes no type) | `wire  path` |
| `info` fields | Scale unit, Simulator, Sim date | Format, Size, Scopes, Index location | Timescale, Date, Version |
| `freq` bit-width | declared `[lbit:rbit]` | inferred from value width | declared width |
| Per-query latency | sub-second | ~0.9 s floor | linear file scan (no cache yet) |

All backends **accept** either `/`- or `.`-separated paths on input; only the
output uses the database's native separator. When quoting a resolved path back
to the user, use what the tool printed - do not convert it.

VCD (`.vcd` / `.vcd.gz`) needs no vendor toolchain. It has no persistent index,
so there is no `--refresh` semantics and no "first query is slow" warning to
give the user; every query re-parses the file.

**`freq` and `debug` pick the shallowest scope** whose name contains the
keyword (closest to the design top). If two scopes at the same depth both
match, the tie is broken by traversal order (FSDB: file order; SHM: sorted
index order), which is not meaningful to the user - so if the wrong instance
comes back, re-run with a more specific keyword or a longer path fragment
rather than assuming the block is missing.

One semantic difference worth knowing: on FSDB, `value` walks back to the last
change before the requested time however distant; on SHM it reads the value the
database reports at the start of a 1 ns window. Results agreed everywhere it was
tested, but if a `value` result on a long-idle signal (a config register, a
tied-off strap) looks wrong, cross-check with `changes` over a wider window
(guardrail **G2**).


## Guidelines

- **If the user gives a full/exact signal path, use it directly on `value`/`changes` as the very first call.** Do not run `scopes`/`signals` "just to check" beforehand — that's wasted digression when the tool already does an instant exact-match lookup on a literal path (dot-separated or slash-separated, either works — see Signal matching above). Only fall back to `scopes`/`signals` if that direct call fails (`Signal not found` / non-zero exit) or if the path is a pattern with more than one match. This applies even when there could be multiple instances (e.g. multiple `blk_<N>` instances) — an exact literal path picks exactly one signal, no scanning needed; ambiguity only exists once you switch to pattern-based search.
- **Use `scopes` first** when unsure about hierarchy (i.e. the user gave a bare name or partial path, not a full exact one). Run `scopes core` before guessing paths — this prevents wrong matches.
- **Use scoped search** (`'core clk'`) for natural-language queries like "clock at core". This is the fastest path when no full path is known.
- **`[N]` array indices in patterns are matched literally**, not as a POSIX character class — `'*blk_inst[0]*'` correctly matches the literal signal name. You don't need to work around this.
- **Check the `Resolved:` line** in stderr to verify the tool matched the correct signal. Report this path to the user.
- Start at the block's top level — don't search deep into hierarchy unnecessarily.
- When multiple matches exist, the tool prints a numbered list and exits with code 1. Present the list to the user and ask which one they want, then re-run with the full path. **Never pick for the user.**
- Use the shallowest matching block — don't require users to provide full hierarchical paths.
- Keep the workflow to as few commands as possible.
- **Never fabricate signal paths.** If `scopes` or `signals` returns nothing, report that to the user — do not guess or invent a path. A non-zero exit code means the query failed; do not present fabricated results. (Guardrails **G1**, **G3** in [`skills/GUARDRAILS.md`](skills/GUARDRAILS.md).)
- **Read-only.** This skill queries the waveform and reports. Do **not** edit, create, or delete RTL, testbench, constraint, config, or any source file, and do **not** launch a simulation, regression, or build while acting under it. If the evidence points to a fix, describe it and stop — applying it is a separate request the user makes explicitly, outside this skill. (Guardrail **G4**.)
- If a full path (even after the automatic normalization above) genuinely returns "not found", the hierarchy segment above the known block likely differs in this testbench. Re-resolve the known block via `scopes` to find the candidate new prefix, but **ask the user to confirm that candidate before querying with it** — don't silently swap in a guessed prefix and present its result as if it were what they asked for.
