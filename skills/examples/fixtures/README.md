<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Shravya Juluru -->

# Fixtures

## `counter.vcd`

A tiny, fully open VCD — no vendor tools, no simulator needed. A testbench `tb`
wrapping a DUT `tb.dut`:

| Signal | Width | Behaviour |
|---|---|---|
| `tb.clk`, `tb.dut.clk` | 1 | 100 MHz (10 ns period), first rising edge at 5 ns |
| `tb.rst_n`, `tb.dut.rst_n` | 1 | active-low, released at 20 ns |
| `tb.dut.count` | 8 | free-running, +1 each rising edge out of reset |
| `tb.dut.valid` | 1 | high once `count >= 3` |
| `tb.dut.ready` | 1 | toggles with `count % 3` |
| `tb.dut.data_out` | 8 | latches `count` when `valid && ready` |
| dump length | | 0 – 205 ns |

It exists so every `wfm_query` command has something real to hit, and so the
contract can be checked in CI without a licensed host.

### Reproduce it

`counter.vcd` is generated, and the generator's output is byte-identical to the
committed file:

```bash
python3 gen_counter_vcd.py > counter.vcd
```

Edit `gen_counter_vcd.py`, not `counter.vcd`, and regenerate.

### Try every command

```bash
WF=skills/examples/fixtures/counter.vcd
wfm_query $WF info
wfm_query $WF scopes dut
wfm_query $WF signals count
wfm_query $WF value tb.dut.count 60          # -> 00000100 (0x04)
wfm_query $WF value /tb/dut/count 95         # slash path also works -> 0x08
wfm_query $WF changes tb.dut.count 40 120
wfm_query $WF freq dut                       # -> clk 100.0 MHz (period 10.000 ns)
wfm_query $WF debug dut 50 -w 20
printf 'info\nfreq dut\n' | wfm_query $WF batch
```

## Third-party VCDs (not committed)

For broader testing, small public VCD/FST corpora exist — e.g. the `wellen`
project's test suite (the reader behind the Surfer viewer) and GTKWave's sample
dumps. Fetch them on demand into a scratch dir; do not commit them (licensing,
repo weight). A `fetch_external.sh` helper can be added here if that becomes a
routine need.
