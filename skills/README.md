<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Shravya Juluru -->

# Subskills & guardrails

`wfm_query` is a **primitive**: one deterministic, read-only query over a
waveform database. It deliberately has no opinion about *your* reset sequence,
*your* clock-domain rules, or *your* failure-triage runbook. Those live here, as
small files a design or verification team owns and evolves.

This directory is a starting point, not a product. Fork it, delete what does not
apply, and add your own.

```
skills/
  GUARDRAILS.md            the pattern: how to encode a team rule as a checkable
  examples/
    reset-check/           "did reset deassert cleanly across these blocks?"
    cdc-triage/            "given a failure time, dump both clock domains"
    failure-triage/        a house runbook: info -> freq -> debug -> changes
    vcd-smoke/             run the fixture through every command; check the contract
    fixtures/              counter.vcd + its generator (open, no vendor tools)
```

## How this composes

- **The base skill** ([`../SKILL.md`](../SKILL.md), installed as `wavegrep`)
  teaches an agent the raw command surface and the safe defaults.
- **A subskill** here is a second `SKILL.md` that assumes the base skill and
  encodes one workflow — which commands, in what order, with which
  project-specific block names and thresholds baked in.
- **A guardrail** ([`GUARDRAILS.md`](GUARDRAILS.md)) is a rule the agent must
  not violate — e.g. "never report a `value` on a long-idle signal without a
  `changes` cross-check." It is enforced by wording in the skill and, where it
  matters, by a wrapper script that exits non-zero when the rule is broken.

Everything rests on [`../CONTRACT.md`](../CONTRACT.md): stable output format,
honest exit codes, `Resolved:` echo. Because the primitive cannot fabricate a
value or mutate anything, your rules only have to cover *interpretation* — not
whether the tool lied.

## Installing a subskill

Copy the subskill directory into your agent's skills path alongside `wavegrep`,
e.g. for Claude Code:

```bash
cp -r skills/examples/failure-triage ~/.claude/skills/wavegrep-failure-triage
```

Then edit its `SKILL.md`: replace the placeholder block names, times, and
thresholds with yours. A subskill with your real hierarchy in it is worth ten
generic ones.

## Contributing one back

See [`../CONTRIBUTING.md`](../CONTRIBUTING.md). Good subskills are
self-contained, call only `wfm_query`, never fabricate a path, and state which
`WFM_*` env vars they assume.
