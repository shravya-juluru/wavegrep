# wavegrep

**A verifiable primitive for building your own DV agents.**

`wavegrep` gives an AI agent (or a shell script, or you) exactly one
capability: read **signal values, clock frequency, transitions, and block
activity** out of Synopsys **FSDB**, Cadence **SST2/SHM**, and open **VCD**
waveform databases — with deterministic output, real exit codes, and nothing
leaving the host. No viewer, no cloud; the VCD path needs no vendor tools at
all.

It is deliberately *not* an autonomous verification engineer. It is the small,
auditable layer such an engineer would stand on. Design and verification teams
compose it into their own **subskills** and **guardrails**: a reset-sequence
check, a clock-domain-crossing triage, a house failure-triage runbook — each one
a short file that calls `wfm_query` and encodes *your* rules, your hierarchy,
your thresholds. See [Extending it](#extending-it-subskills--guardrails).

Ships with a [Claude Code](https://docs.claude.com/en/docs/claude-code) skill
and a dependency-free [MCP server](#use-it-from-an-agent) so it works with the
agent you already use.

Built by [@shravya-juluru](https://github.com/shravya-juluru).

**Names:** the repo and the Claude Code skill are both `wavegrep` (invoke the
skill with `/wavegrep`); the command you run is `wfm_query`.

> **Platform.** The VCD backend is pure Python and runs anywhere. The two
> **vendor** backends (FSDB, SHM) are Linux x86_64 only — they depend on EDA
> toolchains that ship Linux libraries, and `fsdb_query` links Linux-only
> system libraries.

> **Vendor dependencies (not included).** This repo is source + glue only.
> The FSDB backend must be compiled against a licensed **Synopsys Verdi**
> install (`ffrAPI.h`, `libnffr`, `libnsys` — proprietary, not
> redistributable). The SST2 backend shells out to **Cadence Xcelium**
> (`sst2report`, `simvisdbutil`). You supply both. CI cannot build or
> exercise the FSDB path without a Verdi license.

---

## What it's for

You already know your block. For a given failure, or a feature you're bringing
up, you know which signals tell the story — the handshake that should have
fired, the clock that should be toggling, the FIFO that shouldn't have
overflowed. The slow part is *getting the values out*: open the viewer, find
the scope, add the signals, scroll to the time, read them off.

`wavegrep` is that step and nothing more. Point it at a waveform, name the
signal and the time, get the value — from the shell, a script, or an AI agent
you already use. No viewer, no VCD-to-text dump, no new tool to learn.

- **Low effort for an engineer who's already set up.** It's a CLI plus a
  skill / MCP server — drop it in a script or your agent's toolbox. It uses the
  FSDB / SST2 tools you already license; the VCD path needs nothing at all.
- **It has no opinion about your design.** No coverage closure, no bug hunting.
  You bring the context; it fetches the data. Output is deterministic and exit
  codes are honest (see [`CONTRACT.md`](CONTRACT.md)), so an agent calling it
  can't invent a signal value — the tool is itself a guardrail.
- **Nothing leaves your environment.** No RTL, no waveforms, no hierarchy names
  go anywhere. It runs entirely on your host.
- **No lock-in.** Claude Code today, any MCP client tomorrow, a bare shell
  script if that's all you want.

---

## Quick start

### VCD — zero setup, any OS with Python 3

No clone, no build, no `chmod`. Get the files however you like and run:

```bash
python3 wfm_query skills/examples/fixtures/counter.vcd info
python3 wfm_query skills/examples/fixtures/counter.vcd freq dut
python3 wfm_query your_dump.vcd value /tb/dut/clk 1000
```

`python3 wfm_query …` always works — the executable bit is not required. Any
waveform your Verilator / Icarus / GHDL / VCS / Xcelium run can dump as `.vcd`
(usually two lines: `$dumpfile` / `$dumpvars`) is fair game.

### FSDB / SHM — one setup step, Linux + your EDA tools

```bash
git clone https://github.com/shravya-juluru/wavegrep && cd wavegrep
./setup.sh          # got a ZIP instead of a clone? run:  bash setup.sh
```

`setup.sh` checks the platform and Python, `chmod +x`'s the frontends (a ZIP
drops that bit — running it once fixes it), builds `fsdb_query` if a Synopsys
Verdi install is found, checks the Cadence Xcelium tools the SHM backend needs,
and prints what is ready. Safe to re-run; missing vendor tools are reported,
not fatal (the FSDB, SHM, and VCD paths are independent). Point it at
non-default installs with
`./setup.sh --verdi-home /path/to/verdi --xcelium-root /path/to/xcelium/tools`
(or export `VERDI_HOME` / `WFM_XCELIUM_ROOT`). On macOS/Windows it still sets up
the VCD path and reports the vendor backends as unavailable.

---

## Usage

```
wfm_query <waveform> <command> [args]
```

Written `wfm_query …` below for brevity; `python3 wfm_query …` is equivalent and
never needs the executable bit. `<waveform>` is a `.fsdb` file, a `.shm`
directory / `.trn` file, or a `.vcd` / `.vcd.gz` file; `wfm_query` dispatches to
the right backend by format. All times are in **nanoseconds**.

```bash
wfm_query waves.fsdb info
wfm_query waves.fsdb freq core
wfm_query waves.fsdb value /top/dut/core_0/clk 1000
wfm_query waves.fsdb changes /top/dut/data 500 2000
wfm_query dump.shm  debug core_top 5213000 -w 50

# VCD works with no vendor tools — try it now against the bundled fixture:
wfm_query skills/examples/fixtures/counter.vcd info
wfm_query skills/examples/fixtures/counter.vcd freq dut
wfm_query skills/examples/fixtures/counter.vcd value tb.dut.count 95

wfm_query --version                # prints the output-contract version
```

The output format, exit codes, and `Resolved:` convention are a **stable,
versioned interface** — see [`CONTRACT.md`](CONTRACT.md). Build on what it marks
guaranteed; do not parse the rest.

---

## Use it from an agent

### Claude Code skill

Drop this directory in as a Claude Code skill (`.claude/skills/wavegrep/`) and
invoke it with a waveform path:

```
/wavegrep /path/to/waves.fsdb
/wavegrep /path/to/waves.shm
```

Then ask naturally — "what's the clock frequency of core?", "value of signal X
at 1000ns?", "transitions on Y between 500 and 1000ns?", "debug pipe at 2000ns".
[`SKILL.md`](SKILL.md) maps that intent onto the commands above.

### MCP server (any MCP client)

[`mcp_server.py`](mcp_server.py) is a standard-library-only stdio MCP server
that exposes the command surface as typed tools (`wfm_info`, `wfm_scopes`,
`wfm_signals`, `wfm_value`, `wfm_changes`, `wfm_freq`, `wfm_debug`). Add it to
any MCP client (Claude Desktop, Claude Code, Cursor, Cline, …):

```json
{
  "mcpServers": {
    "wavegrep": {
      "command": "python3",
      "args": ["/abs/path/to/wavegrep/mcp_server.py"],
      "env": { "WFM_MCP_WAVEFORM": "/abs/path/to/waves.shm" }
    }
  }
}
```

`WFM_MCP_WAVEFORM` is optional — set it to pin one waveform so callers can omit
the path. The server adds no interpretation; the contract carries straight
through.

---

## Extending it: subskills & guardrails

The base skill teaches an agent the raw commands and safe defaults. Your
team's workflows and rules live in [`skills/`](skills/README.md) as small files
you own:

| | What it is | Ships as |
|---|---|---|
| **Subskill** | One workflow — which commands, in what order, with your block names and thresholds baked in. | a second `SKILL.md` under `skills/examples/` |
| **Guardrail** | A rule the agent must not break, enforced by skill wording and (where it matters) a wrapper that exits non-zero. | [`skills/GUARDRAILS.md`](skills/GUARDRAILS.md) |

Worked examples in the repo, meant to be copied and edited:

- [`skills/examples/reset-check/`](skills/examples/reset-check/SKILL.md) — did
  reset deassert cleanly across a named set of blocks?
- [`skills/examples/cdc-triage/`](skills/examples/cdc-triage/SKILL.md) — given a
  failure time, line up activity in both clock domains of a crossing.
- [`skills/examples/failure-triage/`](skills/examples/failure-triage/SKILL.md) —
  a fixed-order house runbook: `info` → `freq` → `debug` → `changes`.

Because the primitive can't fabricate a value or mutate anything, your rules
only have to cover *interpretation*. See
[`CONTRIBUTING.md`](CONTRIBUTING.md) to send one back.

---

## Supported formats

| Input | Backend | Source | Vendor tools |
|---|---|---|---|
| `*.fsdb` | `fsdb_query` | VCS simulation | Synopsys Verdi (build-time) |
| `*.shm` dir / `*.trn` file | `shm_query` | Cadence SimVision | Cadence Xcelium (run-time) |
| `*.vcd` / `*.vcd.gz` | `vcd_query` | Verilator, Icarus, GHDL, nvc, cocotb, EDA Playground | **none** — pure Python |

`wfm_query` picks the backend from the file extension (and sniffs an
extensionless file). All three expose an identical command surface and take all
times in **nanoseconds**.

VWDB-generation Cadence databases (no `.trn` inside the `.shm`) are **not**
supported — they need `vwdbReport`/`vwdbExtract` instead.

---

## Files

| File | Description |
|---|---|
| `setup.sh` | One-shot Linux setup: checks, builds `fsdb_query`, reports readiness |
| `wfm_query` | Format dispatcher — the entry point |
| `fsdb_query.cpp` | FSDB backend source (build with `make`) |
| `fsdb_query` | FSDB backend binary — **not committed**; produced by `make` against your Verdi install |
| `shm_query` | SST2 backend (Python 3, no build step) |
| `vcd_query` | VCD backend (Python 3, standard library only, no vendor tools) |
| `mcp_server.py` | stdio MCP server wrapping `wfm_query` (stdlib only) |
| `Makefile` | Build script for `fsdb_query` |
| `SKILL.md` | Claude Code skill definition (the `wavegrep` base skill) |
| `CONTRACT.md` | The versioned output contract agents/subskills build on |
| `skills/` | Subskill gallery + `GUARDRAILS.md` + `examples/fixtures/` (the VCD fixture) |
| `tests/` | `test_contract.py` — end-to-end contract check on the VCD path |
| `TESTING.md` | Walkthrough for validating the FSDB/SHM backends on a real waveform |
| `CONTRIBUTING.md` | How to add a subskill or a backend fix |

---

## Requirements

The default tool paths below are **examples from one site's layout**. They are
almost certainly wrong for yours — set the environment variables in
[Environment overrides](#environment-overrides) to point at your installs.

- **FSDB backend**: a licensed Synopsys Verdi providing the FsdbReader kit
  (`ffrAPI.h` + `libnffr`/`libnsys`). Build path defaults to
  `/tools/Synopsys/verdi/X-2025.06`; override with `make VERDI_HOME=...`.
  Verified against Verdi X-2025.06.
- **SHM backend**: Cadence Xcelium providing `sst2report` and `simvisdbutil`.
  Runtime path defaults to `/tools/Cadence/XCELIUM2409`; override with
  `WFM_XCELIUM_ROOT`. XCELIUM2209 and 2409 were both verified to give identical
  results. The tool calls these by absolute path (never via `PATH`) and sets
  `LD_LIBRARY_PATH` internally, so no Cadence shell setup is required.
- **VCD backend**: nothing. `vcd_query` is standard-library Python 3 — no build,
  no vendor tools. Reads `.vcd` and gzip'd `.vcd.gz`.
- **Python** 3.6+ for `wfm_query`, `shm_query`, `vcd_query`, and `mcp_server.py`.

---

## Performance notes (SHM)

The first `.shm` query builds a hierarchy index (a few seconds); later queries
are ~1 s. Per-query cost is a fixed floor, so extra signals are nearly free —
prefer one `debug <block>` over several `value` calls. Always give `changes` a
time window on large dumps.

---

## Backend consistency

All three backends produce **identical output formatting**, so a result reads
the same whichever one ran (this is the [`CONTRACT.md`](CONTRACT.md) surface):

- timestamps in decimal ns — 3 places at or above 1 ns, 6 places below
- values as `<binary> (0x<hex>)`
- same headers and totals for `changes`, `freq`, `scopes`, `signals`
- exit codes: `0` ok, `1` failed / multi-match, `2` usage error
- `debug` applies the same interesting-signal name filter (clk / valid / ready /
  req / ack / err / done / busy / stall / ...)

Differences that remain are inherent to the formats or the backends:

| | FSDB | SHM (SST2) | VCD |
|---|---|---|---|
| Scope separator in output | `/top/dut/core_0/clk` | `chip_top.soc_u0...` | `tb.dut.clk` (dot) |
| `signals` type column | `wire  /path` | path only — SST2 exposes no type | `wire  path` |
| `info` fields | Scale unit, Simulator, Sim date | Format, Size, Scopes, Index path | Timescale, Date, Version |
| `freq` bit-width | declared `[lbit:rbit]` | inferred from value width | declared width |
| Per-query latency | sub-second | ~0.9 s floor | linear scan of the file |
| `debug` interesting-signal list | compiled in (see `is_debug_signal_name` in `fsdb_query.cpp`) | `WFM_DEBUG_KEYWORDS`, overridable | `WFM_DEBUG_KEYWORDS`, overridable |
| `debug` signal count | uncapped | capped at `WFM_DEBUG_CAP` (40) | capped at `WFM_DEBUG_CAP` (40) |
| hierarchy index cache | in-memory per run | on-disk, per-user, auto-invalidated | none — re-parsed per invocation |

All three **accept** `/`- or `.`-separated paths on input; only output uses the
database's native separator.

**Scope selection for `freq` and `debug`.** All pick the *shallowest* scope
whose name contains the keyword (closest to the design top). Ties — two scopes
at the same depth both matching — are broken by traversal order: FSDB uses the
order signals appear in the file, SHM uses sorted index order, VCD uses
`$scope` declaration order. Pass a more specific keyword, or a longer path
fragment, if the wrong instance is chosen.

`value` differs in mechanism: FSDB walks back to the last change before the
requested time however distant, while SHM reads the value reported at the start
of a 1 ns window. These agreed in all testing, but on a long-idle signal (config
register, tied-off strap) cross-check with `changes` over a wider window
(guardrail **G2** in [`skills/GUARDRAILS.md`](skills/GUARDRAILS.md)).

## Environment overrides

| Variable | Default | Purpose |
|---|---|---|
| `WFM_XCELIUM_ROOT` | `/tools/Cadence/XCELIUM2409/tools.lnx86` | Xcelium install for the SHM backend |
| `WFM_SCRATCH_ROOT` | `/scratch/wavegrep` | Parent directory for per-user cache (`$WFM_SCRATCH_ROOT/$USER/wfm_cache`) |
| `WFM_CACHE_DIR` | (derived from above, or `$TMPDIR`, or `~/.cache/wavegrep`) | Hierarchy index cache (~160 MB per dump) |
| `WFM_SIGNALS_MAX` | `100` (FSDB, VCD) / `50` (SHM) | Default max results for `signals` command |
| `WFM_CHANGES_MAX` | `1000` | Default max value changes shown |
| `WFM_DEBUG_CAP` | `40` | Max signals collected by `debug` (SHM and VCD; FSDB does not cap this). |
| `WFM_DEBUG_TRANS_CAP` | `20` | **FSDB, VCD.** Max transitions shown per signal in `debug`. |
| `WFM_FREQ_EDGES` | `3` | **FSDB, VCD.** Rising edges to sample for frequency measurement. |
| `WFM_FREQ_WINDOW_NS` | `100.0` | **SHM only.** Time window for frequency sampling. |
| `WFM_DEBUG_KEYWORDS` | `clk,valid,ready,rdy,enable,...` | **SHM, VCD.** Comma-separated keywords for the interesting-signal filter. The FSDB filter is compiled into `is_debug_signal_name` in `fsdb_query.cpp`. |
| `WFM_MCP_WAVEFORM` | (unset) | **MCP server only.** Pins a default waveform so tool calls may omit the path. |
| `VERDI_HOME` | `/tools/Synopsys/verdi/X-2025.06` | Verdi install (used by Makefile; override with `make VERDI_HOME=...`) |

The cache root is **per-user** — it resolves via `$USER`, falling back to
`$TMPDIR/wfm_cache-$USER` and then `~/.cache/wavegrep` if that path isn't
writable. Nothing is shared between engineers, so there are no permission
collisions.

## Index cache

The first query on a `.shm` runs `sst2report -dsn` once and stores the result as
a flat text index (`signals.txt`, `scopes.txt`, `meta.json`). Every later
`scopes`/`signals` query and every signal-name resolution reads that index
instead of reopening the 160 MB database — which is what turns a ~6 s first
query into ~1 s repeats.

**Regenerating a waveform does not require any manual cache action.** The cache
key includes the `.trn` mtime and size, so a rebuilt dump is detected and
re-indexed automatically, and the superseded index is deleted in the same pass
(matched on the recorded `.trn` path, so other databases' indexes are never
touched). Use `--refresh` only to force a rebuild when the file itself did not
change:

```bash
wfm_query /path/to/waves.shm --refresh info
```

---

## Testing

The VCD path runs with no vendor tools, so it is what CI exercises:

```bash
python3 tests/test_contract.py          # 15 checks against CONTRACT.md
python3 skills/examples/fixtures/gen_counter_vcd.py | diff - skills/examples/fixtures/counter.vcd
```

`tests/test_contract.py` shells the real `wfm_query` against
`skills/examples/fixtures/counter.vcd` and asserts the guaranteed behaviour —
exit codes, the `Resolved:` line, value/timestamp formatting, stable
headers/totals. CI also runs `ruff` and `shellcheck` (lint only — neither
vendor backend can be built there).

The FSDB and SHM backends implement the same contract but need a licensed host
to verify. [`TESTING.md`](TESTING.md) is the walkthrough for validating them
against a real waveform (cross-check every answer against Verdi/SimVision or
`sim.log`); report results with the **Backend validation report** issue form.

## Building the FSDB binary

`./setup.sh` does this for you. To build it directly (only the FSDB backend
needs a build; `wfm_query`, `shm_query`, `vcd_query`, and `mcp_server.py` are
Python):

```bash
make                              # uses VERDI_HOME=/tools/Synopsys/verdi/X-2025.06
make VERDI_HOME=/path/to/verdi    # or point it at your install
```

This produces the `fsdb_query` executable next to the sources. It is
`.gitignore`d — rebuild it per checkout and per Verdi version.

## Publishing this repo

Ship exactly this set — nothing else in the working directory (no `progress.md`,
no `*.docx`):

```
.gitattributes  .gitignore  .github/  LICENSE  NOTICE  README.md  SKILL.md
CONTRACT.md  CONTRIBUTING.md  TESTING.md  DISCOVERABILITY.md  CITATION.cff
Makefile  ruff.toml  setup.sh  wfm_query  shm_query  vcd_query  mcp_server.py
fsdb_query.cpp  skills/  tests/
```

### With git

```bash
git add .gitattributes .gitignore .github LICENSE NOTICE README.md SKILL.md \
        CONTRACT.md CONTRIBUTING.md TESTING.md DISCOVERABILITY.md CITATION.cff \
        Makefile ruff.toml setup.sh wfm_query shm_query vcd_query mcp_server.py \
        fsdb_query.cpp skills tests
git update-index --chmod=+x wfm_query shm_query vcd_query setup.sh \
        mcp_server.py skills/examples/fixtures/gen_counter_vcd.py \
        tests/test_contract.py
git commit -m "Initial commit"
git ls-files --stage | grep -E 'wfm_query|shm_query|vcd_query|setup.sh|mcp_server' # expect mode 100755
```

`.gitattributes` forces LF on all text files so the shebangs stay valid on Linux.

### Without git (web upload / drag-and-drop)

The files are already LF, so shebangs are fine. Plain upload **cannot carry the
executable bit**, so on a fresh Linux checkout run the frontends through the
interpreter once, or let `setup.sh` fix the bits:

```bash
bash setup.sh          # step 3 does: chmod +x wfm_query shm_query vcd_query mcp_server.py
# thereafter: ./wfm_query ...   (or always: python3 wfm_query ...)
```

### GitHub metadata

Set on the repo for discoverability:

- **Description:** A verifiable CLI + MCP/Claude-Code primitive for building your
  own chip-verification agents — query Synopsys FSDB, Cadence SST2, and open VCD
  waveforms (signal values, clock frequency, transitions, block debug). Local,
  no cloud.
- **Topics** (GitHub caps at 20): `fsdb`, `sst2`, `vcd`, `waveform`, `verilog`,
  `systemverilog`, `hardware-verification`, `design-verification`,
  `rtl-simulation`, `eda`, `verdi`, `xcelium`, `simvision`, `cli`, `ai-agents`,
  `agentic-ai`, `mcp`, `claude-code`, `claude-skill`, `llm`

See [DISCOVERABILITY.md](DISCOVERABILITY.md) for the full checklist (lists to
submit to, demo GIF, blog cross-link).

## License

[Apache-2.0](LICENSE). Copyright 2026 Shravya Juluru. See [NOTICE](NOTICE) for
attribution and the vendor-tool disclaimer.
