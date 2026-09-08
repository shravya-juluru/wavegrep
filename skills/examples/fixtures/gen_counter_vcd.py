#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Shravya Juluru
"""
gen_counter_vcd.py - reproduce skills/examples/fixtures/counter.vcd byte-for-byte.

A tiny, fully open VCD: a testbench `tb` wrapping a DUT `tb.dut` with a 100 MHz
clock, an active-low reset released at 20 ns, an 8-bit free-running counter, and
a valid/ready handshake. No vendor tools, no simulator - just enough signal
variety that every wfm_query command has something real to hit.

    python3 gen_counter_vcd.py > counter.vcd
"""

import sys

# id codes (printable ASCII, VCD-legal)
IDS = {
    "tb.clk": "!",
    "tb.rst_n": '"',
    "tb.dut.clk": "#",
    "tb.dut.rst_n": "$",
    "tb.dut.count": "%",
    "tb.dut.valid": "&",
    "tb.dut.ready": "'",
    "tb.dut.data_out": "(",
}

T_END = 200          # ns
CLK_HALF = 5         # ns  -> 10 ns period, 100 MHz
RST_RELEASE = 20     # ns


def bits(value, width):
    return format(value & ((1 << width) - 1), "0%db" % width)


def main():
    out = sys.stdout
    w = out.write

    w("$date\n    Tue Sep  1 00:00:00 2026\n$end\n")
    w("$version\n    gen_counter_vcd.py (wavegrep fixture)\n$end\n")
    w("$timescale 1ns $end\n")
    w("$scope module tb $end\n")
    w('$var wire 1 %s clk $end\n' % IDS["tb.clk"])
    w('$var wire 1 %s rst_n $end\n' % IDS["tb.rst_n"])
    w("$scope module dut $end\n")
    w('$var wire 1 %s clk $end\n' % IDS["tb.dut.clk"])
    w('$var wire 1 %s rst_n $end\n' % IDS["tb.dut.rst_n"])
    w('$var wire 8 %s count [7:0] $end\n' % IDS["tb.dut.count"])
    w('$var wire 1 %s valid $end\n' % IDS["tb.dut.valid"])
    w('$var wire 1 %s ready $end\n' % IDS["tb.dut.ready"])
    w('$var wire 8 %s data_out [7:0] $end\n' % IDS["tb.dut.data_out"])
    w("$upscope $end\n")
    w("$upscope $end\n")
    w("$enddefinitions $end\n")

    # ---- state ----
    clk = 0
    rst_n = 0
    count = 0
    valid = 0
    ready = 0
    data_out = 0

    def emit_scalar(key, v):
        w("%d%s\n" % (v, IDS[key]))

    def emit_vec(key, v, width):
        w("b%s %s\n" % (bits(v, width), IDS[key]))

    # ---- initial dump at time 0 ----
    w("$dumpvars\n")
    emit_scalar("tb.clk", clk)
    emit_scalar("tb.rst_n", rst_n)
    emit_scalar("tb.dut.clk", clk)
    emit_scalar("tb.dut.rst_n", rst_n)
    emit_vec("tb.dut.count", count, 8)
    emit_scalar("tb.dut.valid", valid)
    emit_scalar("tb.dut.ready", ready)
    emit_vec("tb.dut.data_out", data_out, 8)
    w("$end\n")

    # ---- walk time in CLK_HALF steps ----
    t = 0
    while t <= T_END:
        lines = []

        rising = False
        if t > 0 and t % CLK_HALF == 0:
            clk ^= 1
            rising = (clk == 1)
            lines.append("%d%s" % (clk, IDS["tb.clk"]))
            lines.append("%d%s" % (clk, IDS["tb.dut.clk"]))

        if t == RST_RELEASE and rst_n == 0:
            rst_n = 1
            lines.append("%d%s" % (rst_n, IDS["tb.rst_n"]))
            lines.append("%d%s" % (rst_n, IDS["tb.dut.rst_n"]))

        # rising clock edge, out of reset: advance the counter and the handshake
        if rising and rst_n == 1:
            count = (count + 1) & 0xFF
            lines.append("b%s %s" % (bits(count, 8), IDS["tb.dut.count"]))

            new_valid = 1 if count >= 3 else 0
            if new_valid != valid:
                valid = new_valid
                lines.append("%d%s" % (valid, IDS["tb.dut.valid"]))

            new_ready = 1 if (count % 3) != 0 else 0
            if new_ready != ready:
                ready = new_ready
                lines.append("%d%s" % (ready, IDS["tb.dut.ready"]))

            if valid and ready and data_out != count:
                data_out = count
                lines.append("b%s %s" % (bits(data_out, 8), IDS["tb.dut.data_out"]))

        if lines:
            w("#%d\n" % t)
            for ln in lines:
                w(ln + "\n")

        t += CLK_HALF

    w("#%d\n" % (T_END + CLK_HALF))


if __name__ == "__main__":
    main()
