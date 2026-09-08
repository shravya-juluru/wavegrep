#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Shravya Juluru
"""
mcp_server.py - Model Context Protocol (stdio) wrapper around wfm_query.

Exposes the wfm_query command surface as MCP tools so any MCP client
(Claude Desktop, Claude Code, Cursor, Cline, ...) can query FSDB / SST2 / VCD
waveforms - not just the bundled Claude Code skill.

It is a thin, dependency-free shim: every tool call shells out to
`wfm_query <waveform> <command> ...`, and returns stdout plus the
`Resolved:` line unchanged. The determinism and exit-code guarantees in
CONTRACT.md carry through untouched - this file adds no interpretation.

Run directly:  python3 mcp_server.py
Client config (example, Claude Desktop / Claude Code mcpServers block):

  {
    "mcpServers": {
      "wavegrep": {
        "command": "python3",
        "args": ["/abs/path/to/mcp_server.py"],
        "env": { "WFM_MCP_WAVEFORM": "/abs/path/to/waves.shm" }
      }
    }
  }

WFM_MCP_WAVEFORM (optional) pins a default waveform so callers may omit the
`waveform` argument. All the WFM_* / VERDI_HOME / WFM_XCELIUM_ROOT env vars
that wfm_query reads are inherited as-is.
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
WFM_QUERY = os.path.join(HERE, "wfm_query")

PROTOCOL_VERSION = "2025-06-18"
SERVER_INFO = {"name": "wavegrep", "version": "1.0"}
DEFAULT_WAVEFORM = os.environ.get("WFM_MCP_WAVEFORM", "")

# --- tool definitions -------------------------------------------------------
# Each entry: wfm_query subcommand + how to turn JSON arguments into argv.
# Keep this table aligned with CONTRACT.md and SKILL.md's command mapping.

_WAVEFORM_PROP = {
    "type": "string",
    "description": "Path to a .fsdb file, a .shm dir / .trn file, or a .vcd[.gz] "
                   "file. Omit to use the server's pinned default (WFM_MCP_WAVEFORM).",
}


def _time_prop(desc):
    return {"type": "number", "description": desc + " (nanoseconds)"}


TOOLS = {
    "wfm_info": {
        "description": "Waveform file metadata: signal count, scope count, "
                       "format, simulator/date where available.",
        "properties": {"waveform": _WAVEFORM_PROP},
        "required": [],
        "argv": lambda a: ["info"],
    },
    "wfm_scopes": {
        "description": "List design scopes (blocks/instances) whose name "
                       "contains a keyword. Use this before guessing a path.",
        "properties": {
            "waveform": _WAVEFORM_PROP,
            "keyword": {"type": "string", "description": "Substring to match against scope names."},
        },
        "required": ["keyword"],
        "argv": lambda a: ["scopes", str(a["keyword"])],
    },
    "wfm_signals": {
        "description": "List signals matching a pattern (bare name -> substring; "
                       "glob supported; 'scope signal' does a scoped search).",
        "properties": {
            "waveform": _WAVEFORM_PROP,
            "pattern": {"type": "string", "description": "Name, glob, or 'scope signal'. Default '*'."},
            "max": {"type": "integer", "description": "Max results.", "minimum": 1},
        },
        "required": [],
        "argv": lambda a: ["signals", str(a.get("pattern", "*"))]
        + (["-n", str(a["max"])] if a.get("max") is not None else []),
    },
    "wfm_value": {
        "description": "Value of one signal at one time. Returns "
                       "'<binary> (0x<hex>)'; the resolved full path is reported too.",
        "properties": {
            "waveform": _WAVEFORM_PROP,
            "signal": {"type": "string", "description": "Full path (fastest), bare name, glob, or 'scope signal'."},
            "time": _time_prop("Sample time"),
        },
        "required": ["signal", "time"],
        "argv": lambda a: ["value", str(a["signal"]), _ns(a["time"])],
    },
    "wfm_changes": {
        "description": "Value transitions on a signal, optionally within a "
                       "[t0, t1] window. Always pass a window on large dumps.",
        "properties": {
            "waveform": _WAVEFORM_PROP,
            "signal": {"type": "string", "description": "Full path, bare name, glob, or 'scope signal'."},
            "t0": _time_prop("Window start (optional)"),
            "t1": _time_prop("Window end (optional)"),
            "max": {"type": "integer", "description": "Max transitions shown.", "minimum": 1},
        },
        "required": ["signal"],
        "argv": lambda a: ["changes", str(a["signal"])]
        + ([_ns(a["t0"])] if a.get("t0") is not None else [])
        + ([_ns(a["t1"])] if a.get("t1") is not None else [])
        + (["-n", str(a["max"])] if a.get("max") is not None else []),
    },
    "wfm_freq": {
        "description": "Measured clock frequency/period for the shallowest scope "
                       "matching <block>. Pass a longer fragment to disambiguate.",
        "properties": {
            "waveform": _WAVEFORM_PROP,
            "block": {"type": "string", "description": "Block/scope keyword or path fragment."},
        },
        "required": ["block"],
        "argv": lambda a: ["freq", str(a["block"])],
    },
    "wfm_debug": {
        "description": "Snapshot of the interesting (handshake/clk/valid/...) "
                       "signals in a block around a time - the best-value command "
                       "on SHM (extra signals are nearly free).",
        "properties": {
            "waveform": _WAVEFORM_PROP,
            "block": {"type": "string", "description": "Block/scope keyword or path fragment."},
            "time": _time_prop("Centre of the debug window"),
            "window": {"type": "number", "description": "Half-window in ns (default per backend)."},
        },
        "required": ["block", "time"],
        "argv": lambda a: ["debug", str(a["block"]), _ns(a["time"])]
        + (["-w", _ns(a["window"])] if a.get("window") is not None else []),
    },
}


def _ns(v):
    """Render a JSON number as wfm_query expects: integer when whole."""
    f = float(v)
    return str(int(f)) if f.is_integer() else repr(f)


# --- wfm_query invocation --------------------------------------------------

def run_wfm(name, args):
    spec = TOOLS[name]
    waveform = args.get("waveform") or DEFAULT_WAVEFORM
    if not waveform:
        return True, ("No waveform given and WFM_MCP_WAVEFORM is not set. "
                      "Pass 'waveform', or pin one in the server config.")
    # Invoke through the interpreter: wfm_query is a Python script, and this
    # works the same whether or not the exec bit survived the checkout.
    cmd = [sys.executable, WFM_QUERY, str(waveform)] + [str(x) for x in spec["argv"](args)]

    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except FileNotFoundError:
        return True, "wfm_query not found next to mcp_server.py"
    except subprocess.TimeoutExpired:
        return True, "wfm_query timed out after 300s"
    except OSError as e:
        return True, "could not run wfm_query: %s" % e

    out = p.stdout.rstrip("\n")
    err = p.stderr.rstrip("\n")
    # stderr carries the `Resolved:` line and any diagnostics - keep both,
    # but label them so the model does not confuse them with result data.
    body = out
    if err:
        body = (body + "\n" if body else "") + "--- stderr ---\n" + err
    if p.returncode != 0:
        body = ("wfm_query exited %d (see CONTRACT.md: 1 = failure or multi-match, "
                "2 = usage).\n\n" % p.returncode) + body
    return p.returncode != 0, body or "(no output)"


# --- MCP stdio plumbing (newline-delimited JSON-RPC 2.0) -----------------

def _tool_list():
    return [
        {
            "name": name,
            "description": spec["description"],
            "inputSchema": {
                "type": "object",
                "properties": spec["properties"],
                "required": spec["required"],
                "additionalProperties": False,
            },
        }
        for name, spec in TOOLS.items()
    ]


def handle(msg):
    mid = msg.get("id")
    method = msg.get("method", "")
    params = msg.get("params") or {}

    if method == "initialize":
        return _ok(mid, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": SERVER_INFO,
        })
    if method in ("notifications/initialized", "initialized"):
        return None
    if method == "ping":
        return _ok(mid, {})
    if method == "tools/list":
        return _ok(mid, {"tools": _tool_list()})
    if method == "tools/call":
        name = params.get("name")
        if name not in TOOLS:
            return _err(mid, -32602, "unknown tool: %s" % name)
        is_error, text = run_wfm(name, params.get("arguments") or {})
        return _ok(mid, {"content": [{"type": "text", "text": text}], "isError": is_error})
    if mid is None:
        return None  # unknown notification - ignore
    return _err(mid, -32601, "method not found: %s" % method)


def _ok(mid, result):
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def _err(mid, code, message):
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}


def main():
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        try:
            reply = handle(msg)
        except Exception as e:  # never let one bad call kill the server
            reply = _err(msg.get("id"), -32603, "internal error: %s" % e)
        if reply is not None:
            out.write(json.dumps(reply) + "\n")
            out.flush()


if __name__ == "__main__":
    main()
