<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Shravya Juluru -->

# Guardrails

A guardrail is a rule an agent must not break while using `wfm_query`. The
strongest guardrails are not a wrapper bolted around the agent — they are the
same checks a senior engineer would apply, written down where the agent reads
them and, where it matters, enforced by a script that fails fast.

This file is the pattern plus the guardrails that ship with the base skill.
Copy it, add yours.

---

## Anatomy of a guardrail

1. **The rule**, in one sentence.
2. **Why** — the failure it prevents.
3. **How the agent honours it** — wording placed in the relevant `SKILL.md`.
4. **How it is enforced** (optional) — a wrapper script that exits non-zero
   when the rule is violated, so a violation cannot pass silently.

Levels 1–3 are always worth doing. Level 4 is worth it for rules where a
plausible-looking wrong answer is dangerous.

---

## Shipped guardrails

### G1 — No answer after a non-zero exit

**Rule.** If `wfm_query` exits non-zero, the agent must not present a signal
value, frequency, or transition list as fact.

**Why.** Exit `1` means *failure or multi-match*; exit `2` means *bad
arguments*. `stdout` in those cases is a diagnostic or a candidate list, not an
answer. Reporting it as an answer is how a hallucinated value reaches a bug
report.

**How the agent honours it.** The base skill says: *"A non-zero exit code means
the query failed; do not present fabricated results."* On a multi-match, show
the numbered list and ask which one.

**Enforcement.** Trivial and worth it — check `$?`.

---

### G2 — Cross-check `value` on long-idle signals

**Rule.** Before reporting a `value` on a signal that may have been static for a
long time (config register, tied-off strap, mode pin), run `changes` over a
wide window and confirm the value is consistent.

**Why.** FSDB `value` walks back to the last transition however distant; SHM
`value` reads what the database reports at the start of a 1 ns window. They
agreed in all testing, but the mechanisms differ, so a surprising `value` on a
long-idle signal deserves a second look before it lands in a conclusion.

**How the agent honours it.** Wording in `../SKILL.md` under "Guidelines".

**Enforcement.** See `examples/failure-triage/` for a wrapper that runs the
`changes` cross-check automatically whenever `value` is used on a signal outside
an active-traffic window.

---

### G3 — Never fabricate or guess a hierarchy path

**Rule.** If `scopes` / `signals` returns nothing, the agent reports that. It
does not invent a path, and it does not silently swap in a guessed hierarchy
prefix when a known-good path stops resolving.

**Why.** A fabricated path that happens to resolve to *some* signal produces a
confident, wrong answer — the worst failure mode in verification.

**How the agent honours it.** Wording in `../SKILL.md`: re-resolve the known
block via `scopes`, present the candidate prefix, and **ask the user to confirm
it** before querying with it.

---

### G4 — Read-only engagement: never modify RTL, testbench, or any source

**Rule.** While acting under this skill the agent's job is to *query the
waveform and report*. It must not edit, create, delete, patch, or `git`-mutate
any RTL, testbench, constraint, config, or other source file — and must not run
a simulation, regression, or build. If the investigation suggests a fix, the
agent describes it and stops; applying it is a separate, explicit request the
user makes outside this skill.

**Why.** A waveform-debug skill is often invoked in a session where the agent
*also* holds file-edit and shell tools. "Helpfully" patching the RTL from a
waveform reading is how a wrong inference becomes a silent source change. The
value of this tool is that it sits between the engineer and ground truth
*without* also being in the edit path — keep it that way.

**How the agent honours it.** Wording in `../SKILL.md` under "Guidelines". The
underlying tool cannot write to the design in any case: `wfm_query` opens
waveforms read-only and writes only its own per-user index cache (see
[`../CONTRACT.md`](../CONTRACT.md) § Determinism). G4 is the constraint on the
*agent around* the tool, which the tool alone cannot enforce.

**Enforcement.** If the harness supports it, run this skill with file-write and
VCS tools denied for the duration. Otherwise it is a review point: a diff to any
source file produced during a `/wavegrep` investigation is a G4 violation.

---

## Team guardrails — write your own

Examples of rules a specific project might add:

- **G-local-1.** `debug` / `freq` may only be run against blocks on an
  approved-block list — fail if the keyword resolves outside it. (Keeps an agent
  out of encrypted or third-party IP subtrees.)
- **G-local-2.** Every reported time must be echoed back with its `sim.log`
  cycle number, computed from the measured clock period — never a bare ns value.
- **G-local-3.** No query may run against a waveform under `/scratch` older than
  the latest regression tag; stale-waveform answers are worse than no answer.

Encode each as G1–G3 above: sentence, why, skill wording, optional wrapper.
