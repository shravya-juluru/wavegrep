<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Shravya Juluru -->

# Contributing

Two kinds of contribution are especially welcome:

1. **Subskills and guardrails** — workflows and rules that sit on top of
   `wfm_query`. These are the point of the project; see
   [`skills/`](skills/README.md).
2. **Backend fixes** — correctness or portability fixes to `fsdb_query.cpp`,
   `shm_query`, or `wfm_query`.

## Ground rules for a subskill

A subskill is a directory under `skills/examples/` (or your own tree) with a
`SKILL.md` carrying `name:` and `description:` frontmatter. To be mergeable:

- **Self-contained.** It calls only `wfm_query`. No other binaries, no network.
- **Honest about config.** Anything site-specific (block names, times,
  thresholds, `WFM_*` env vars it assumes) is listed in a `CONFIG` section at
  the top with clearly-marked placeholders — never real internal identifiers.
- **Contract-abiding.** It relies only on what [`CONTRACT.md`](CONTRACT.md)
  marks *guaranteed*: exit codes, the `Resolved:` line, the `<binary> (0x<hex>)`
  value format. It does not parse column positions or prose wording.
- **Guardrail-clean.** It never fabricates or guesses a hierarchy path (G3),
  never reports a result after a non-zero exit (G1), and cross-checks a `value`
  on a long-idle signal (G2). See [`skills/GUARDRAILS.md`](skills/GUARDRAILS.md).
- **No vendor material.** Do not include Synopsys or Cadence headers, output
  captured from their tools, or real design hierarchy from a proprietary chip.

## Ground rules for a backend change

- Keep all three backends' **observable behaviour identical** — same output
  format, same exit codes, same `Resolved:` convention (see
  [`CONTRACT.md`](CONTRACT.md)). A change that only makes sense for one format
  still has to degrade cleanly on the others.
- No new runtime dependencies for the Python paths (`wfm_query`, `shm_query`,
  `vcd_query`, `mcp_server.py` stay standard-library only).
- **`vcd_query` changes must keep `tests/test_contract.py` green** and, if they
  affect output, update it in the same PR. It is the one backend that runs with
  no vendor tools.
- FSDB / SHM changes can't be verified without a vendor license — describe how
  you tested (tool version, a real `.fsdb` / `.shm`, the commands run and their
  output). [`TESTING.md`](TESTING.md) is the walkthrough; attach the evidence in
  the PR or an issue.
- If you change the fixture, regenerate it from
  `skills/examples/fixtures/gen_counter_vcd.py` (never hand-edit `counter.vcd`)
  and keep the two byte-identical.
- Run `ruff check` on the Python and `shellcheck` on `setup.sh` before opening
  the PR.

## Legal

By contributing you agree your contribution is licensed under
[Apache-2.0](LICENSE). Add an
`SPDX-License-Identifier: Apache-2.0` header to any new source file. You retain
copyright to your contribution; add yourself to a file header if you wish.
