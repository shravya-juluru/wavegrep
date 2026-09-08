// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Shravya Juluru
//
// fsdb_query.cpp - Command-line FSDB waveform query tool
// Uses Synopsys FsdbReader API to query signals without full VCD conversion.
// Designed to scale to large (1.5GB+) FSDB files:
//   - Streaming tree traversal: signals are matched/printed during traversal,
//     not stored in a big map. This keeps memory usage low for 14M+ signal designs.
//   - Only requested signals are loaded for value queries.
//   - Incremental scope path, pre-computed lowercase, early exit on match.

#ifdef NOVAS_FSDB
#undef NOVAS_FSDB
#endif

#include "ffrAPI.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fnmatch.h>

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

// ---- Tunable defaults (override via environment) ----
static int    g_signals_max   = 100;   // WFM_SIGNALS_MAX
static int    g_changes_max   = 1000;  // WFM_CHANGES_MAX
static int    g_debug_trans   = 20;    // WFM_DEBUG_TRANS_CAP
static int    g_freq_edges    = 3;     // WFM_FREQ_EDGES

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v ? atoi(v) : def;
}

static void init_config() {
    g_signals_max  = env_int("WFM_SIGNALS_MAX",     100);
    g_changes_max  = env_int("WFM_CHANGES_MAX",    1000);
    g_debug_trans  = env_int("WFM_DEBUG_TRANS_CAP",   20);
    g_freq_edges   = env_int("WFM_FREQ_EDGES",        3);
}

// ---- Minimal per-signal info (only stored when needed) ----

struct SignalInfo {
    std::string full_path;
    fsdbVarIdcode idcode;
    int lbit;
    int rbit;
    byte_T type;
    byte_T bpb;
};

// ---- Incremental scope path (avoids rebuilding from stack on every signal) ----

static std::string g_scope_path;
static std::vector<size_t> g_scope_lengths;

static void scope_push(const char *name) {
    g_scope_lengths.push_back(g_scope_path.size());
    g_scope_path += '/';
    g_scope_path += name;
}

static void scope_pop() {
    if (!g_scope_lengths.empty()) {
        g_scope_path.resize(g_scope_lengths.back());
        g_scope_lengths.pop_back();
    }
}

static void scope_reset() {
    g_scope_path.clear();
    g_scope_lengths.clear();
}

static inline void to_lower_inplace(std::string &s) {
    for (auto &c : s) c = tolower((unsigned char)c);
}

// ---- Time conversion: all I/O in nanoseconds ----

static double g_units_per_ns = 1.0; // FSDB internal units per nanosecond

static void init_time_scale(ffrObject *obj) {
    const char *su = obj->ffrGetScaleUnit();
    if (!su) { g_units_per_ns = 1e6; return; } // default: assume fs

    // Parse scale unit string like "1 fs", "1 ps", "1 ns", "100 ps", etc.
    double digit = 1.0;
    char unit[16] = "";
    sscanf(su, "%lf %15s", &digit, unit);

    double fs_per_unit = digit;
    if (strcmp(unit, "fs") == 0)      fs_per_unit = digit;
    else if (strcmp(unit, "ps") == 0) fs_per_unit = digit * 1e3;
    else if (strcmp(unit, "ns") == 0) fs_per_unit = digit * 1e6;
    else if (strcmp(unit, "us") == 0) fs_per_unit = digit * 1e9;
    else if (strcmp(unit, "ms") == 0) fs_per_unit = digit * 1e12;
    else if (strcmp(unit, "s") == 0)  fs_per_unit = digit * 1e15;
    else                              fs_per_unit = digit; // unknown, assume fs

    // 1 ns = 1e6 fs. units_per_ns = 1e6 / fs_per_unit
    g_units_per_ns = 1e6 / fs_per_unit;
}

static uint64_t ns_to_internal(double ns) {
    return (uint64_t)(ns * g_units_per_ns + 0.5);
}

static double internal_to_ns(uint64_t t) {
    return (double)t / g_units_per_ns;
}

static std::string format_time_ns(uint64_t t) {
    double ns = internal_to_ns(t);
    char buf[64];
    if (ns >= 1.0)
        snprintf(buf, sizeof(buf), "%.3f", ns);
    else
        snprintf(buf, sizeof(buf), "%.6f", ns);
    return std::string(buf);
}

// ---- Time range validation ----

static uint64_t g_sim_min = 0, g_sim_max = 0;
static int g_exit_code = 0;

static void init_sim_range(ffrObject *obj) {
    fsdbTag64 min_tag, max_tag;
    obj->ffrGetMinFsdbTag64(&min_tag);
    obj->ffrGetMaxFsdbTag64(&max_tag);
    g_sim_min = ((uint64_t)min_tag.H << 32) | min_tag.L;
    g_sim_max = ((uint64_t)max_tag.H << 32) | max_tag.L;
}

static void warn_time_range(uint64_t t) {
    if (t > g_sim_max) {
        fprintf(stderr, "WARNING: requested time %s ns is beyond simulation end (%s ns)\n",
                format_time_ns(t).c_str(), format_time_ns(g_sim_max).c_str());
    }
}

// ---- Signal width helper ----

static int sig_width(int lbit, int rbit) {
    if (lbit == 0 && rbit == 0) return 1;
    int w = lbit - rbit;
    if (w < 0) w = -w;
    return w + 1;
}

// ---- Streaming callback context ----

enum CbMode {
    CB_MODE_NONE,        // Don't store anything (info command)
    CB_MODE_SIGNALS,     // Print matching signals during traversal (streaming)
    CB_MODE_FIND_ONE,    // Find a specific signal by path/pattern, store its idcode
    CB_MODE_FIND_SCOPE,  // Find scopes matching a keyword
    CB_MODE_SCOPE_CLKS,  // Find clock signals at a specific scope's direct level
    CB_MODE_SCOPED_FIND, // Two-phase: find scope by keyword, then find signal within it
    CB_MODE_FREQ,        // Single-pass: find scope + collect clocks at shallowest match
    CB_MODE_SCOPE_SIGS,  // Collect key debug signals at a specific scope
    CB_MODE_DEBUG_SCAN,  // Single-pass: find scope + collect debug signals at shallowest match
};

enum FindSubMode {
    FIND_EXACT,      // Full path starting with /
    FIND_SUBSTRING,  // Auto-wrapped *name* — use fast substring instead of fnmatch
    FIND_GLOB,       // User-provided glob pattern
};

struct CbContext {
    CbMode mode;

    // For CB_MODE_SIGNALS:
    const char *pattern;
    int match_count;
    int max_results;

    // For CB_MODE_FIND_ONE:
    const char *find_path;
    std::string find_path_lower;  // pre-computed lowercase
    std::string find_substr_lower; // for FIND_SUBSTRING: the bare keyword (no *'s)
    FindSubMode find_submode;
    fsdbVarIdcode found_idcode;
    std::string found_path;
    int found_lbit;
    int found_rbit;
    byte_T found_type;
    byte_T found_bpb;
    int find_matches;
    std::vector<SignalInfo> find_first_10;

    // For CB_MODE_FIND_SCOPE:
    std::string scope_keyword_lower;
    std::vector<std::string> found_scopes;

    // For CB_MODE_SCOPE_CLKS / CB_MODE_SCOPE_SIGS:
    std::string target_scope;
    bool in_target_scope;
    int target_scope_depth;
    std::vector<SignalInfo> scope_clks;
    std::vector<SignalInfo> scope_sigs;

    // For CB_MODE_SCOPED_FIND:
    std::string sf_scope_kw;
    std::string sf_sig_kw;
    std::string sf_best_scope;
    bool sf_phase2;
    bool sf_in_scope;
    int sf_scope_depth;
    std::vector<SignalInfo> sf_matches;

    // For CB_MODE_FREQ (single-pass):
    std::string freq_scope_kw_lower;
    std::string freq_best_scope;
    bool freq_collecting;
    int freq_collect_depth;
    std::vector<SignalInfo> freq_clks;

    // For CB_MODE_DEBUG_SCAN (single-pass: find scope + collect debug signals):
    std::string debug_scope_kw_lower;
    std::string debug_best_scope;
    bool debug_collecting;
    int debug_collect_depth;
};

static CbContext g_ctx;

static const char* var_type_str(byte_T type) {
    switch (type) {
    case FSDB_VT_VCD_REG:       return "reg";
    case FSDB_VT_VCD_WIRE:      return "wire";
    case FSDB_VT_VCD_INTEGER:   return "integer";
    case FSDB_VT_VCD_REAL:      return "real";
    case FSDB_VT_VCD_PARAMETER: return "parameter";
    case FSDB_VT_VCD_EVENT:     return "event";
    case FSDB_VT_VCD_SUPPLY0:   return "supply0";
    case FSDB_VT_VCD_SUPPLY1:   return "supply1";
    case FSDB_VT_VCD_TRI:       return "tri";
    case FSDB_VT_VCD_MEMORY:    return "memory";
    case FSDB_VT_VCD_TIME:      return "time";
    default:                    return "other";
    }
}

static bool is_debug_signal_name(const std::string &name_lower) {
    return name_lower.find("clk") != std::string::npos ||
           name_lower.find("valid") != std::string::npos ||
           name_lower.find("ready") != std::string::npos ||
           name_lower.find("rdy") != std::string::npos ||
           name_lower.find("enable") != std::string::npos ||
           name_lower.find("_en") != std::string::npos ||
           (name_lower.size() >= 2 && name_lower[0] == 'e' && name_lower[1] == 'n') ||
           name_lower.find("data") != std::string::npos ||
           name_lower.find("err") != std::string::npos ||
           name_lower.find("req") != std::string::npos ||
           name_lower.find("ack") != std::string::npos ||
           name_lower.find("grant") != std::string::npos ||
           name_lower.find("done") != std::string::npos ||
           name_lower.find("busy") != std::string::npos ||
           name_lower.find("stall") != std::string::npos;
}

// ---- Streaming tree callback ----

static bool_T tree_cb(fsdbTreeCBType cb_type, void *client_data, void *tree_cb_data) {
    int depth = (int)g_scope_lengths.size();

    switch (cb_type) {
    case FSDB_TREE_CBT_SCOPE: {
        fsdbTreeCBDataScope *scope = (fsdbTreeCBDataScope*)tree_cb_data;
        scope_push(scope->name);

        if (g_ctx.mode == CB_MODE_FIND_SCOPE) {
            std::string name_lower(scope->name);
            to_lower_inplace(name_lower);
            if (name_lower.find(g_ctx.scope_keyword_lower) != std::string::npos)
                g_ctx.found_scopes.push_back(g_scope_path);
        }
        else if (g_ctx.mode == CB_MODE_SCOPE_CLKS || g_ctx.mode == CB_MODE_SCOPE_SIGS) {
            if (g_scope_path == g_ctx.target_scope) {
                g_ctx.in_target_scope = true;
                g_ctx.target_scope_depth = depth + 1;
            }
        }
        else if (g_ctx.mode == CB_MODE_SCOPED_FIND) {
            if (!g_ctx.sf_phase2) {
                std::string name_lower(scope->name);
                to_lower_inplace(name_lower);
                if (name_lower.find(g_ctx.sf_scope_kw) != std::string::npos) {
                    if (g_ctx.sf_best_scope.empty() || g_scope_path.size() < g_ctx.sf_best_scope.size())
                        g_ctx.sf_best_scope = g_scope_path;
                }
            } else {
                if (!g_ctx.sf_in_scope && g_scope_path == g_ctx.sf_best_scope) {
                    g_ctx.sf_in_scope = true;
                    g_ctx.sf_scope_depth = depth + 1;
                }
            }
        }
        else if (g_ctx.mode == CB_MODE_FREQ) {
            if (!g_ctx.freq_collecting) {
                std::string name_lower(scope->name);
                to_lower_inplace(name_lower);
                if (name_lower.find(g_ctx.freq_scope_kw_lower) != std::string::npos) {
                    if (g_ctx.freq_best_scope.empty() || g_scope_path.size() < g_ctx.freq_best_scope.size()) {
                        g_ctx.freq_best_scope = g_scope_path;
                        g_ctx.freq_collecting = true;
                        g_ctx.freq_collect_depth = depth + 1;
                        g_ctx.freq_clks.clear();
                    }
                }
            }
        }
        else if (g_ctx.mode == CB_MODE_DEBUG_SCAN) {
            if (!g_ctx.debug_collecting) {
                std::string name_lower(scope->name);
                to_lower_inplace(name_lower);
                if (name_lower.find(g_ctx.debug_scope_kw_lower) != std::string::npos) {
                    if (g_ctx.debug_best_scope.empty() || g_scope_path.size() < g_ctx.debug_best_scope.size()) {
                        g_ctx.debug_best_scope = g_scope_path;
                        g_ctx.debug_collecting = true;
                        g_ctx.debug_collect_depth = depth + 1;
                        g_ctx.scope_sigs.clear();
                    }
                }
            }
        }
        break;
    }
    case FSDB_TREE_CBT_UPSCOPE: {
        if ((g_ctx.mode == CB_MODE_SCOPE_CLKS || g_ctx.mode == CB_MODE_SCOPE_SIGS) &&
            g_ctx.in_target_scope && depth == g_ctx.target_scope_depth) {
            g_ctx.in_target_scope = false;
            scope_pop();
            return FALSE;
        }
        if (g_ctx.mode == CB_MODE_SCOPED_FIND && g_ctx.sf_phase2 &&
            g_ctx.sf_in_scope && depth == g_ctx.sf_scope_depth) {
            g_ctx.sf_in_scope = false;
            scope_pop();
            return FALSE;
        }
        if (g_ctx.mode == CB_MODE_FREQ && g_ctx.freq_collecting && depth == g_ctx.freq_collect_depth) {
            g_ctx.freq_collecting = false;
            scope_pop();
            return FALSE;
        }
        if (g_ctx.mode == CB_MODE_DEBUG_SCAN && g_ctx.debug_collecting && depth == g_ctx.debug_collect_depth) {
            g_ctx.debug_collecting = false;
            scope_pop();
            return FALSE;
        }
        scope_pop();
        break;
    }
    case FSDB_TREE_CBT_VAR: {
        if (g_ctx.mode == CB_MODE_NONE) break;
        if (g_ctx.mode == CB_MODE_FIND_SCOPE) break;
        if (g_ctx.mode == CB_MODE_SCOPED_FIND && !g_ctx.sf_phase2) break;

        fsdbTreeCBDataVar *var = (fsdbTreeCBDataVar*)tree_cb_data;

        if (g_ctx.mode == CB_MODE_SIGNALS) {
            std::string path = g_scope_path + "/" + var->name;
            bool match = true;
            if (g_ctx.pattern && strlen(g_ctx.pattern) > 0) {
                match = (fnmatch(g_ctx.pattern, path.c_str(), FNM_CASEFOLD) == 0);
            }
            if (match) {
                if (g_ctx.max_results > 0 && g_ctx.match_count >= g_ctx.max_results) {
                    g_ctx.match_count++;
                    return FALSE;
                }
                if (sig_width(var->lbitnum, var->rbitnum) == 1)
                    printf("%-8s  %s\n", var_type_str(var->type), path.c_str());
                else
                    printf("%-8s  [%d:%d]  %s\n", var_type_str(var->type),
                           var->lbitnum, var->rbitnum, path.c_str());
                g_ctx.match_count++;
            }
        }
        else if (g_ctx.mode == CB_MODE_SCOPE_CLKS) {
            if (!g_ctx.in_target_scope) break;
            std::string name_lower(var->name);
            to_lower_inplace(name_lower);
            if (name_lower.find("clk") != std::string::npos) {
                SignalInfo si;
                si.full_path = g_scope_path + "/" + var->name;
                si.idcode = var->u.idcode;
                si.lbit = var->lbitnum;
                si.rbit = var->rbitnum;
                si.type = var->type;
                si.bpb = var->bytes_per_bit;
                g_ctx.scope_clks.push_back(si);
            }
        }
        else if (g_ctx.mode == CB_MODE_SCOPE_SIGS) {
            if (!g_ctx.in_target_scope) break;
            std::string name_lower(var->name);
            to_lower_inplace(name_lower);
            if (is_debug_signal_name(name_lower)) {
                SignalInfo si;
                si.full_path = g_scope_path + "/" + var->name;
                si.idcode = var->u.idcode;
                si.lbit = var->lbitnum;
                si.rbit = var->rbitnum;
                si.type = var->type;
                si.bpb = var->bytes_per_bit;
                g_ctx.scope_sigs.push_back(si);
            }
        }
        else if (g_ctx.mode == CB_MODE_FREQ) {
            if (!g_ctx.freq_collecting) break;
            std::string name_lower(var->name);
            to_lower_inplace(name_lower);
            if (name_lower.find("clk") != std::string::npos) {
                SignalInfo si;
                si.full_path = g_scope_path + "/" + var->name;
                si.idcode = var->u.idcode;
                si.lbit = var->lbitnum;
                si.rbit = var->rbitnum;
                si.type = var->type;
                si.bpb = var->bytes_per_bit;
                g_ctx.freq_clks.push_back(si);
            }
        }
        else if (g_ctx.mode == CB_MODE_DEBUG_SCAN) {
            if (!g_ctx.debug_collecting) break;
            std::string name_lower(var->name);
            to_lower_inplace(name_lower);
            if (is_debug_signal_name(name_lower)) {
                SignalInfo si;
                si.full_path = g_scope_path + "/" + var->name;
                si.idcode = var->u.idcode;
                si.lbit = var->lbitnum;
                si.rbit = var->rbitnum;
                si.type = var->type;
                si.bpb = var->bytes_per_bit;
                g_ctx.scope_sigs.push_back(si);
            }
        }
        else if (g_ctx.mode == CB_MODE_SCOPED_FIND && g_ctx.sf_phase2) {
            if (!g_ctx.sf_in_scope) break;
            std::string name_lower(var->name);
            to_lower_inplace(name_lower);
            if (name_lower.find(g_ctx.sf_sig_kw) != std::string::npos) {
                SignalInfo si;
                si.full_path = g_scope_path + "/" + var->name;
                si.idcode = var->u.idcode;
                si.lbit = var->lbitnum;
                si.rbit = var->rbitnum;
                si.type = var->type;
                si.bpb = var->bytes_per_bit;
                g_ctx.sf_matches.push_back(si);
            }
        }
        else if (g_ctx.mode == CB_MODE_FIND_ONE) {
            // Lazy path construction: check var name first for fast reject
            const std::string &fl = g_ctx.find_path_lower;

            if (g_ctx.find_submode == FIND_SUBSTRING) {
                // Fast path: check if var name contains the keyword before building full path
                std::string name_lower(var->name);
                to_lower_inplace(name_lower);
                if (name_lower.find(g_ctx.find_substr_lower) == std::string::npos) break;
            }

            std::string path = g_scope_path + "/" + var->name;
            bool match = false;

            if (g_ctx.find_submode == FIND_EXACT) {
                std::string path_lower = path;
                to_lower_inplace(path_lower);
                match = (path_lower == fl);
            }
            else if (g_ctx.find_submode == FIND_SUBSTRING) {
                // Already passed the name check above — verify full path contains keyword
                std::string path_lower = path;
                to_lower_inplace(path_lower);
                match = (path_lower.find(g_ctx.find_substr_lower) != std::string::npos);
            }
            else { // FIND_GLOB
                // Try suffix match first (cheaper than fnmatch)
                std::string path_lower = path;
                to_lower_inplace(path_lower);
                if (path_lower.size() >= fl.size() &&
                    path_lower.compare(path_lower.size() - fl.size(), fl.size(), fl) == 0) {
                    match = true;
                }
                if (!match) {
                    match = (fnmatch(g_ctx.find_path, path.c_str(), FNM_CASEFOLD) == 0);
                }
            }

            if (match) {
                g_ctx.find_matches++;
                if (g_ctx.find_matches == 1) {
                    g_ctx.found_idcode = var->u.idcode;
                    g_ctx.found_path = path;
                    g_ctx.found_lbit = var->lbitnum;
                    g_ctx.found_rbit = var->rbitnum;
                    g_ctx.found_type = var->type;
                    g_ctx.found_bpb = var->bytes_per_bit;
                }
                if (g_ctx.find_first_10.size() < 10) {
                    SignalInfo si;
                    si.full_path = path;
                    si.idcode = var->u.idcode;
                    si.lbit = var->lbitnum;
                    si.rbit = var->rbitnum;
                    si.type = var->type;
                    si.bpb = var->bytes_per_bit;
                    g_ctx.find_first_10.push_back(si);
                }
                if (g_ctx.find_submode == FIND_EXACT)
                    return FALSE;
                if (g_ctx.find_first_10.size() >= 10)
                    return FALSE;
            }
        }
        break;
    }
    default:
        break;
    }
    return TRUE;
}

// ---- Value formatting ----

static std::string format_vc_1b(byte_T *vc_ptr, uint_T bit_size) {
    std::string result;
    result.reserve(bit_size);
    for (uint_T i = 0; i < bit_size; i++) {
        switch (vc_ptr[i]) {
        case FSDB_BT_VCD_0: result += '0'; break;
        case FSDB_BT_VCD_1: result += '1'; break;
        case FSDB_BT_VCD_X: result += 'x'; break;
        case FSDB_BT_VCD_Z: result += 'z'; break;
        default:            result += '?'; break;
        }
    }
    return result;
}

static std::string format_vc(ffrVCTrvsHdl hdl, byte_T *vc_ptr) {
    uint_T bit_size = hdl->ffrGetBitSize();
    switch (hdl->ffrGetBytesPerBit()) {
    case FSDB_BYTES_PER_BIT_1B:
        return format_vc_1b(vc_ptr, bit_size);
    case FSDB_BYTES_PER_BIT_4B: {
        char buf[64];
        snprintf(buf, sizeof(buf), "%f", *((float*)vc_ptr));
        return std::string(buf);
    }
    case FSDB_BYTES_PER_BIT_8B: {
        char buf[64];
        snprintf(buf, sizeof(buf), "%e", *((double*)vc_ptr));
        return std::string(buf);
    }
    default:
        return "???";
    }
}

static std::string bin_to_hex(const std::string &bin) {
    std::string padded = bin;
    while (padded.size() % 4 != 0) padded = "0" + padded;

    std::string hex;
    for (size_t i = 0; i < padded.size(); i += 4) {
        std::string nibble = padded.substr(i, 4);
        if (nibble.find('x') != std::string::npos) {
            hex += 'x';
        } else if (nibble.find('z') != std::string::npos) {
            hex += 'z';
        } else {
            int val = 0;
            for (int j = 0; j < 4; j++)
                val = val * 2 + (nibble[j] - '0');
            char c[2];
            snprintf(c, sizeof(c), "%x", val);
            hex += c;
        }
    }
    size_t first_nonzero = hex.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return "0";
    return hex.substr(first_nonzero);
}

// ---- Parse time string (input is in ns, convert to internal units) ----

static uint64_t parse_time(const char *str) {
    char *end;
    double val = strtod(str, &end);
    return ns_to_internal(val);
}

// ---- Find signal ----

static bool has_glob_chars(const char *s) {
    // Note: '[' is deliberately NOT treated as a glob indicator here. In hardware
    // signal names, '[' always means a literal array/bit-range index (e.g. "sig[3]",
    // "data[31:0]"), never a POSIX character class. Patterns with only brackets
    // and no '*'/'?' should fall through to plain substring matching, which treats
    // brackets literally with no ambiguity at all.
    for (; *s; s++)
        if (*s == '*' || *s == '?') return true;
    return false;
}

// fnmatch() treats '[' '...' ']' as a character class. That's never what's wanted
// for hardware signal paths -- escape both brackets so they match literally, while
// '*' and '?' keep working as real wildcards.
static std::string escape_brackets_for_fnmatch(const char *pattern) {
    std::string out;
    for (const char *p = pattern; *p; p++) {
        if (*p == '[' || *p == ']') out += '\\';
        out += *p;
    }
    return out;
}

// Heuristic: a bare (no leading '/', no existing '/') dot-separated hierarchical
// path, e.g. "top.dut.core0.fifo.u_wr.data[31:0]" (as commonly seen in UVM log
// messages). Returns the '/'-separated, leading-'/' form
// ready for an exact lookup, or "" if this doesn't look like that shape.
static std::string normalize_dot_path(const char *pattern) {
    std::string s(pattern);
    if (s.find('/') != std::string::npos) return "";
    if (s.find('.') == std::string::npos) return "";
    for (char c : s) {
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '[' || c == ']' || c == ':'))
            return "";
    }
    std::string out = "/";
    for (char c : s) out += (c == '.') ? '/' : c;
    return out;
}

// Run a FIND_EXACT scan for an already '/'-separated path. Returns the match count;
// on 1 match, g_ctx.found_* is populated; on >1, g_ctx.find_first_10 is populated.
static int run_exact_scan(ffrObject *obj, const std::string &exact_path) {
    g_ctx.mode = CB_MODE_FIND_ONE;
    g_ctx.find_submode = FIND_EXACT;
    g_ctx.find_path = exact_path.c_str();
    g_ctx.find_path_lower = exact_path;
    to_lower_inplace(g_ctx.find_path_lower);
    g_ctx.found_idcode = 0;
    g_ctx.found_path.clear();
    g_ctx.find_matches = 0;
    g_ctx.find_first_10.clear();
    scope_reset();
    obj->ffrReadScopeVarTree();
    return g_ctx.find_matches;
}

static bool scoped_find(ffrObject *obj, const std::string &scope_kw, const std::string &sig_kw) {
    g_ctx.mode = CB_MODE_SCOPED_FIND;
    g_ctx.sf_phase2 = false;
    g_ctx.sf_scope_kw = scope_kw;
    g_ctx.sf_sig_kw = sig_kw;
    g_ctx.sf_best_scope.clear();
    g_ctx.sf_matches.clear();
    g_ctx.sf_in_scope = false;
    scope_reset();
    to_lower_inplace(g_ctx.sf_scope_kw);
    to_lower_inplace(g_ctx.sf_sig_kw);

    obj->ffrReadScopeVarTree();

    if (g_ctx.sf_best_scope.empty()) {
        fprintf(stderr, "No scope matching '%s' found.\n", scope_kw.c_str());
        g_exit_code = 1;
        return false;
    }

    fprintf(stderr, "(scoped search: %s -> %s)\n", scope_kw.c_str(), g_ctx.sf_best_scope.c_str());

    g_ctx.sf_phase2 = true;
    g_ctx.sf_in_scope = false;
    scope_reset();

    obj->ffrReadScopeVarTree();

    if (g_ctx.sf_matches.empty()) {
        fprintf(stderr, "No signal matching '%s' found under %s\n", sig_kw.c_str(), g_ctx.sf_best_scope.c_str());
        g_exit_code = 1;
        return false;
    }
    if (g_ctx.sf_matches.size() == 1) {
        g_ctx.found_idcode = g_ctx.sf_matches[0].idcode;
        g_ctx.found_path = g_ctx.sf_matches[0].full_path;
        g_ctx.found_lbit = g_ctx.sf_matches[0].lbit;
        g_ctx.found_rbit = g_ctx.sf_matches[0].rbit;
        g_ctx.found_type = g_ctx.sf_matches[0].type;
        g_ctx.found_bpb = g_ctx.sf_matches[0].bpb;
        g_ctx.find_matches = 1;
        fprintf(stderr, "Resolved: %s\n", g_ctx.found_path.c_str());
        return true;
    }

    fprintf(stderr, "Found %zu signals matching '%s' under %s:\n",
            g_ctx.sf_matches.size(), sig_kw.c_str(), g_ctx.sf_best_scope.c_str());
    for (size_t i = 0; i < g_ctx.sf_matches.size() && i < 10; i++)
        fprintf(stderr, "  %s\n", g_ctx.sf_matches[i].full_path.c_str());
    if (g_ctx.sf_matches.size() > 10)
        fprintf(stderr, "  ... and %zu more\n", g_ctx.sf_matches.size() - 10);

    for (auto &m : g_ctx.sf_matches) {
        std::string scope_path = m.full_path.substr(0, m.full_path.rfind('/'));
        if (scope_path == g_ctx.sf_best_scope) {
            g_ctx.found_idcode = m.idcode;
            g_ctx.found_path = m.full_path;
            g_ctx.found_lbit = m.lbit;
            g_ctx.found_rbit = m.rbit;
            g_ctx.found_type = m.type;
            g_ctx.found_bpb = m.bpb;
            g_ctx.find_matches = 1;
            fprintf(stderr, "Using direct child: %s\n", m.full_path.c_str());
            return true;
        }
    }
    g_ctx.found_idcode = g_ctx.sf_matches[0].idcode;
    g_ctx.found_path = g_ctx.sf_matches[0].full_path;
    g_ctx.found_lbit = g_ctx.sf_matches[0].lbit;
    g_ctx.found_rbit = g_ctx.sf_matches[0].rbit;
    g_ctx.found_type = g_ctx.sf_matches[0].type;
    g_ctx.found_bpb = g_ctx.sf_matches[0].bpb;
    g_ctx.find_matches = 1;
    fprintf(stderr, "Using first match: %s\n", g_ctx.sf_matches[0].full_path.c_str());
    return true;
}

static bool try_scoped_find(ffrObject *obj, const char *pattern) {
    if (!pattern || pattern[0] == '/' || has_glob_chars(pattern))
        return false;

    std::string pat(pattern);
    size_t sp = pat.find(' ');
    if (sp != std::string::npos && sp > 0 && sp < pat.size() - 1) {
        return scoped_find(obj, pat.substr(0, sp), pat.substr(sp + 1));
    }
    return false;
}

static bool find_signal(ffrObject *obj, const char *pattern) {
    if (try_scoped_find(obj, pattern))
        return true;

    // Fast path: a bare dot-separated hierarchical path (no leading '/', no '/' at
    // all) most likely came from a UVM log message or RTL reference, not the
    // FSDB's own '/'-separated form. Try the normalized exact lookup first, before
    // falling into substring/glob matching -- avoids a false "not found" purely
    // from separator mismatch.
    if (pattern && pattern[0] != '/') {
        std::string normalized = normalize_dot_path(pattern);
        if (!normalized.empty()) {
            int n = run_exact_scan(obj, normalized);
            if (n == 1) {
                fprintf(stderr, "Resolved (normalized '.' path to '/'): %s\n", g_ctx.found_path.c_str());
                return true;
            } else if (n > 1) {
                fprintf(stderr, "Multiple matches (%d) for normalized path '%s'. Pick one:\n",
                        n, normalized.c_str());
                for (size_t i = 0; i < g_ctx.find_first_10.size(); i++)
                    fprintf(stderr, "  [%zu] %s\n", i + 1, g_ctx.find_first_10[i].full_path.c_str());
                if (n > (int)g_ctx.find_first_10.size())
                    fprintf(stderr, "  ... and %d more (use a more specific pattern)\n",
                            n - (int)g_ctx.find_first_10.size());
                g_exit_code = 1;
                return false;
            }
            // n == 0: fall through to substring/glob matching below on the original pattern.
        }
    }

    std::string wrapped;
    std::string escaped_glob;
    FindSubMode submode;

    if (pattern && pattern[0] == '/') {
        submode = FIND_EXACT;
    } else if (pattern && !has_glob_chars(pattern)) {
        // Bare name (possibly containing literal '[N]'): use fast substring match
        // instead of fnmatch -- substring comparison treats brackets literally with
        // no ambiguity, so no escaping is needed here.
        submode = FIND_SUBSTRING;
        g_ctx.find_substr_lower = pattern;
        to_lower_inplace(g_ctx.find_substr_lower);
        wrapped = std::string("*") + pattern + "*";
        pattern = wrapped.c_str();
    } else {
        submode = FIND_GLOB;
    }

    g_ctx.mode = CB_MODE_FIND_ONE;
    // find_path_lower drives both the cheap literal-suffix pre-check (for all
    // submodes) and the "not found" message below -- keep it derived from the
    // real (unescaped) pattern so brackets stay literal there. find_path itself is
    // only read by fnmatch() in FIND_GLOB mode, so that's the one that gets the
    // bracket-escaped version.
    g_ctx.find_path_lower = pattern ? pattern : "";
    to_lower_inplace(g_ctx.find_path_lower);
    if (submode == FIND_GLOB) {
        escaped_glob = escape_brackets_for_fnmatch(pattern);
        g_ctx.find_path = escaped_glob.c_str();
    } else {
        g_ctx.find_path = pattern;
    }
    g_ctx.find_submode = submode;
    g_ctx.found_idcode = 0;
    g_ctx.found_path.clear();
    g_ctx.find_matches = 0;
    g_ctx.find_first_10.clear();
    scope_reset();

    obj->ffrReadScopeVarTree();

    if (g_ctx.find_matches == 0) {
        fprintf(stderr, "Signal not found: %s\n", pattern);
        g_exit_code = 1;
        return false;
    }
    if (g_ctx.find_matches > 1) {
        fprintf(stderr, "Multiple matches (%d) for '%s'. Pick one:\n", g_ctx.find_matches, pattern);
        for (size_t i = 0; i < g_ctx.find_first_10.size(); i++)
            fprintf(stderr, "  [%zu] %s\n", i + 1, g_ctx.find_first_10[i].full_path.c_str());
        if (g_ctx.find_matches > (int)g_ctx.find_first_10.size())
            fprintf(stderr, "  ... and %d more (use a more specific pattern)\n",
                    g_ctx.find_matches - (int)g_ctx.find_first_10.size());
        g_exit_code = 1;
        return false;
    }
    fprintf(stderr, "Resolved: %s\n", g_ctx.found_path.c_str());
    return true;
}

// ---- Commands ----

static void cmd_info(ffrObject *obj, const char *fname) {
    fsdbTag64 min_tag, max_tag;
    obj->ffrGetMinFsdbTag64(&min_tag);
    obj->ffrGetMaxFsdbTag64(&max_tag);

    uint64_t t_min = ((uint64_t)min_tag.H << 32) | min_tag.L;
    uint64_t t_max = ((uint64_t)max_tag.H << 32) | max_tag.L;

    printf("File: %s\n", fname);
    printf("Scale unit: %s (all times shown in ns)\n", obj->ffrGetScaleUnit());
    printf("Time range: %s ns - %s ns\n", format_time_ns(t_min).c_str(), format_time_ns(t_max).c_str());
    printf("Total unique signals: %u\n", obj->ffrGetMaxVarIdcode());
    printf("Simulator: %s\n", obj->ffrGetSimVersion());
    printf("Sim date: %s\n", obj->ffrGetSimDate());
    printf("Scope separator: %s\n", obj->ffrGetScopeSeparator());
}

static void cmd_scopes(ffrObject *obj, const char *keyword) {
    g_ctx.mode = CB_MODE_FIND_SCOPE;
    g_ctx.scope_keyword_lower = keyword ? keyword : "";
    to_lower_inplace(g_ctx.scope_keyword_lower);
    g_ctx.found_scopes.clear();
    scope_reset();

    obj->ffrReadScopeVarTree();

    if (g_ctx.found_scopes.empty()) {
        printf("No scopes matching '%s'\n", keyword ? keyword : "(all)");
        return;
    }

    // Sort by depth (shallowest first)
    std::sort(g_ctx.found_scopes.begin(), g_ctx.found_scopes.end(),
              [](const std::string &a, const std::string &b) { return a.size() < b.size(); });

    printf("Scopes matching '%s':\n", keyword ? keyword : "(all)");
    for (size_t i = 0; i < g_ctx.found_scopes.size() && i < 50; i++)
        printf("  %s\n", g_ctx.found_scopes[i].c_str());
    if (g_ctx.found_scopes.size() > 50)
        printf("  ... and %zu more\n", g_ctx.found_scopes.size() - 50);
    printf("\nTotal: %zu scopes\n", g_ctx.found_scopes.size());
}

static void cmd_signals(ffrObject *obj, const char *pattern, int max_results) {
    g_ctx.mode = CB_MODE_SIGNALS;
    // Escape brackets so a literal array index like '[3]' in the pattern is matched
    // literally by fnmatch(), not parsed as a character class (see
    // escape_brackets_for_fnmatch). The escaped string must outlive the tree read
    // below, so it's a local, not a temporary.
    std::string escaped_pattern;
    if (pattern) {
        escaped_pattern = escape_brackets_for_fnmatch(pattern);
        g_ctx.pattern = escaped_pattern.c_str();
    } else {
        g_ctx.pattern = pattern;
    }
    g_ctx.match_count = 0;
    g_ctx.max_results = max_results;
    scope_reset();

    obj->ffrReadScopeVarTree();

    if (max_results > 0 && g_ctx.match_count >= max_results)
        printf("\n(showing first %d, use -n to increase)\n", max_results);
    else
        printf("\nTotal matches: %d\n", g_ctx.match_count);
}

static void cmd_value(ffrObject *obj, const char *sig_path, const char *time_str) {
    if (!find_signal(obj, sig_path)) return;

    fsdbVarIdcode idcode = g_ctx.found_idcode;
    obj->ffrAddToSignalList(idcode);
    obj->ffrLoadSignals();

    ffrVCTrvsHdl hdl = obj->ffrCreateVCTraverseHandle(idcode);
    if (!hdl) {
        fprintf(stderr, "Failed to create traverse handle\n");
        obj->ffrUnloadSignals();
        return;
    }

    uint64_t t = parse_time(time_str);
    warn_time_range(t);
    fsdbTag64 xtag;
    xtag.H = (uint_T)(t >> 32);
    xtag.L = (uint_T)(t & 0xFFFFFFFF);

    byte_T *vc_ptr;
    if (FSDB_RC_SUCCESS == hdl->ffrGotoXTag(&xtag)) {
        if (FSDB_RC_SUCCESS == hdl->ffrGetVC(&vc_ptr)) {
            std::string val = format_vc(hdl, vc_ptr);
            std::string hex = bin_to_hex(val);
            printf("Signal: %s\n", g_ctx.found_path.c_str());
            printf("Time:   %s ns\n", format_time_ns(t).c_str());
            printf("Value:  %s (0x%s)\n", val.c_str(), hex.c_str());
        }
    } else {
        fprintf(stderr, "No value change found at or before time %s ns\n", time_str);
    }

    hdl->ffrFree();
    obj->ffrResetSignalList();
    obj->ffrUnloadSignals();
}

static void cmd_changes(ffrObject *obj, const char *sig_path,
                         const char *begin_str, const char *end_str,
                         int max_changes = g_changes_max) {
    if (!find_signal(obj, sig_path)) return;

    fsdbVarIdcode idcode = g_ctx.found_idcode;

    if (begin_str) {
        uint64_t bt = parse_time(begin_str);
        uint64_t et = end_str ? parse_time(end_str) : g_sim_max;
        warn_time_range(bt);
        if (end_str) warn_time_range(et);
        fsdbXTag start_xtag, close_xtag;
        start_xtag.hltag.H = (uint_T)(bt >> 32);
        start_xtag.hltag.L = (uint_T)(bt & 0xFFFFFFFF);
        close_xtag.hltag.H = (uint_T)(et >> 32);
        close_xtag.hltag.L = (uint_T)(et & 0xFFFFFFFF);
        obj->ffrSetViewWindow(&start_xtag, &close_xtag);
    }

    obj->ffrAddToSignalList(idcode);
    obj->ffrLoadSignals();

    ffrVCTrvsHdl hdl = obj->ffrCreateVCTraverseHandle(idcode);
    if (!hdl) {
        fprintf(stderr, "Failed to create traverse handle\n");
        obj->ffrUnloadSignals();
        return;
    }

    printf("Signal: %s\n", g_ctx.found_path.c_str());
    printf("%-20s  %-40s  %s\n", "Time", "Binary", "Hex");
    printf("%-20s  %-40s  %s\n", "----", "------", "---");

    fsdbTag64 time;
    byte_T *vc_ptr;
    int count = 0;

    if (!hdl->ffrHasIncoreVC()) {
        printf("(no value changes)\n");
    } else {
        hdl->ffrGetMinXTag(&time);
        hdl->ffrGotoXTag(&time);

        do {
            hdl->ffrGetXTag(&time);
            if (FSDB_RC_SUCCESS == hdl->ffrGetVC(&vc_ptr)) {
                std::string val = format_vc(hdl, vc_ptr);
                std::string hex = bin_to_hex(val);
                uint64_t raw_t = ((uint64_t)time.H << 32) | time.L;
                std::string t_ns = format_time_ns(raw_t);
                printf("%-20s  %-40s  0x%s\n", t_ns.c_str(), val.c_str(), hex.c_str());
                count++;
                if (max_changes > 0 && count >= max_changes) {
                    printf("\n(capped at %d, use -n to increase)\n", max_changes);
                    break;
                }
            }
        } while (FSDB_RC_SUCCESS == hdl->ffrGotoNextVC());
    }

    if (max_changes <= 0 || count < max_changes)
        printf("\nTotal: %d value changes\n", count);

    hdl->ffrFree();
    obj->ffrResetSignalList();
    obj->ffrUnloadSignals();

    if (begin_str) {
        fsdbXTag start_xtag, close_xtag;
        start_xtag.hltag.H = 0; start_xtag.hltag.L = 0;
        close_xtag.hltag.H = 0xFFFFFFFF; close_xtag.hltag.L = 0xFFFFFFFF;
        obj->ffrResetViewWindow(&start_xtag, &close_xtag);
    }
}

static void cmd_freq(ffrObject *obj, const char *keyword) {
    // Single-pass: find shallowest scope + collect its clock signals, then early exit
    g_ctx.mode = CB_MODE_FREQ;
    g_ctx.freq_scope_kw_lower = keyword;
    to_lower_inplace(g_ctx.freq_scope_kw_lower);
    g_ctx.freq_best_scope.clear();
    g_ctx.freq_collecting = false;
    g_ctx.freq_clks.clear();
    scope_reset();

    obj->ffrReadScopeVarTree();

    if (g_ctx.freq_best_scope.empty()) {
        fprintf(stderr, "No block matching '%s' found in design hierarchy.\n", keyword);
        g_exit_code = 1;
        return;
    }

    printf("Block: %s\n", g_ctx.freq_best_scope.c_str());

    if (g_ctx.freq_clks.empty()) {
        printf("  No clock signals found at this scope level.\n");
        return;
    }

    // Filter 1-bit clocks and batch-load
    std::vector<size_t> clk_indices;
    for (size_t i = 0; i < g_ctx.freq_clks.size(); i++) {
        auto &clk = g_ctx.freq_clks[i];
        if (sig_width(clk.lbit, clk.rbit) != 1) {
            printf("  %-30s  [%d:%d] (skipped, not 1-bit)\n",
                   clk.full_path.c_str(), clk.lbit, clk.rbit);
        } else {
            obj->ffrAddToSignalList(clk.idcode);
            clk_indices.push_back(i);
        }
    }

    if (clk_indices.empty()) return;
    obj->ffrLoadSignals();

    for (size_t idx : clk_indices) {
        auto &clk = g_ctx.freq_clks[idx];

        ffrVCTrvsHdl hdl = obj->ffrCreateVCTraverseHandle(clk.idcode);
        if (!hdl) {
            printf("  %-30s  (failed to read)\n", clk.full_path.c_str());
            continue;
        }

        fsdbTag64 time;
        byte_T *vc_ptr;
        std::vector<uint64_t> rising_edges;

        if (hdl->ffrHasIncoreVC()) {
            hdl->ffrGetMinXTag(&time);
            hdl->ffrGotoXTag(&time);

            byte_T prev_val = FSDB_BT_VCD_X;
            do {
                if (FSDB_RC_SUCCESS == hdl->ffrGetVC(&vc_ptr)) {
                    byte_T cur_val = vc_ptr[0];
                    if (prev_val == FSDB_BT_VCD_0 && cur_val == FSDB_BT_VCD_1) {
                        hdl->ffrGetXTag(&time);
                        uint64_t t = ((uint64_t)time.H << 32) | time.L;
                        rising_edges.push_back(t);
                        if ((int)rising_edges.size() >= g_freq_edges) break;
                    }
                    prev_val = cur_val;
                }
            } while (FSDB_RC_SUCCESS == hdl->ffrGotoNextVC());
        }

        std::string leaf = clk.full_path;
        size_t last_slash = leaf.rfind('/');
        if (last_slash != std::string::npos) leaf = leaf.substr(last_slash + 1);

        if (rising_edges.size() >= 2) {
            uint64_t period_raw = rising_edges[1] - rising_edges[0];
            double period_ns = internal_to_ns(period_raw);
            double freq_ghz = 1.0 / period_ns;

            if (freq_ghz >= 1.0)
                printf("  %-30s  %.3f GHz  (period %.3f ns)\n",
                       leaf.c_str(), freq_ghz, period_ns);
            else
                printf("  %-30s  %.1f MHz  (period %.3f ns)\n",
                       leaf.c_str(), freq_ghz * 1000.0, period_ns);
        } else {
            printf("  %-30s  (no toggling detected)\n", leaf.c_str());
        }

        hdl->ffrFree();
    }

    obj->ffrResetSignalList();
    obj->ffrUnloadSignals();
}

static void cmd_debug(ffrObject *obj, const char *keyword, const char *time_str, uint64_t window_ns) {
    uint64_t center_time = parse_time(time_str);
    warn_time_range(center_time);
    uint64_t window = ns_to_internal((double)window_ns);
    uint64_t t_begin = (center_time > window) ? center_time - window : 0;
    uint64_t t_end = center_time + window;

    // Single-pass: find shallowest scope + collect debug signals
    g_ctx.mode = CB_MODE_DEBUG_SCAN;
    g_ctx.debug_scope_kw_lower = keyword;
    to_lower_inplace(g_ctx.debug_scope_kw_lower);
    g_ctx.debug_best_scope.clear();
    g_ctx.debug_collecting = false;
    g_ctx.scope_sigs.clear();
    scope_reset();
    obj->ffrReadScopeVarTree();

    if (g_ctx.debug_best_scope.empty()) {
        fprintf(stderr, "No block matching '%s' found in design hierarchy.\n", keyword);
        g_exit_code = 1;
        return;
    }

    printf("=== Debug Summary ===\n");
    printf("Block:  %s\n", g_ctx.debug_best_scope.c_str());
    printf("Time:   %s ns (window: %s - %s ns)\n",
           format_time_ns(center_time).c_str(),
           format_time_ns(t_begin).c_str(),
           format_time_ns(t_end).c_str());
    printf("\n");

    if (g_ctx.scope_sigs.empty()) {
        printf("No key signals (clk/valid/ready/data/err/req/ack) found at scope.\n");
        return;
    }

    printf("Found %zu key signals. Values at time %s ns:\n\n",
           g_ctx.scope_sigs.size(), format_time_ns(center_time).c_str());

    // Batch-load all signals at once
    for (auto &sig : g_ctx.scope_sigs)
        obj->ffrAddToSignalList(sig.idcode);
    obj->ffrLoadSignals();

    // Query value at center_time for each signal
    fsdbTag64 xtag;
    xtag.H = (uint_T)(center_time >> 32);
    xtag.L = (uint_T)(center_time & 0xFFFFFFFF);

    printf("%-40s  %-16s  %s\n", "Signal", "Value", "Hex");
    printf("%-40s  %-16s  %s\n", "------", "-----", "---");

    for (auto &sig : g_ctx.scope_sigs) {
        std::string leaf = sig.full_path;
        size_t last_slash = leaf.rfind('/');
        if (last_slash != std::string::npos) leaf = leaf.substr(last_slash + 1);

        ffrVCTrvsHdl hdl = obj->ffrCreateVCTraverseHandle(sig.idcode);
        if (!hdl) {
            printf("%-40s  (failed to read)\n", leaf.c_str());
            continue;
        }

        byte_T *vc_ptr;
        if (FSDB_RC_SUCCESS == hdl->ffrGotoXTag(&xtag)) {
            if (FSDB_RC_SUCCESS == hdl->ffrGetVC(&vc_ptr)) {
                std::string val = format_vc(hdl, vc_ptr);
                std::string hex = bin_to_hex(val);
                std::string disp_val = val;
                if (disp_val.size() > 16) disp_val = disp_val.substr(0, 13) + "...";
                printf("%-40s  %-16s  0x%s\n", leaf.c_str(), disp_val.c_str(), hex.c_str());
            }
        } else {
            printf("%-40s  (no value)\n", leaf.c_str());
        }

        hdl->ffrFree();
    }

    obj->ffrResetSignalList();
    obj->ffrUnloadSignals();

    // Transitions for narrow control signals in window
    printf("\n--- Transitions in window [%s - %s ns] ---\n\n",
           format_time_ns(t_begin).c_str(), format_time_ns(t_end).c_str());

    fsdbXTag start_xtag, close_xtag;
    start_xtag.hltag.H = (uint_T)(t_begin >> 32);
    start_xtag.hltag.L = (uint_T)(t_begin & 0xFFFFFFFF);
    close_xtag.hltag.H = (uint_T)(t_end >> 32);
    close_xtag.hltag.L = (uint_T)(t_end & 0xFFFFFFFF);

    // Batch-load narrow signals for transition display
    obj->ffrSetViewWindow(&start_xtag, &close_xtag);
    std::vector<size_t> narrow_indices;
    for (size_t i = 0; i < g_ctx.scope_sigs.size(); i++) {
        if (sig_width(g_ctx.scope_sigs[i].lbit, g_ctx.scope_sigs[i].rbit) <= 8) {
            obj->ffrAddToSignalList(g_ctx.scope_sigs[i].idcode);
            narrow_indices.push_back(i);
        }
    }
    obj->ffrLoadSignals();

    for (size_t idx : narrow_indices) {
        auto &sig = g_ctx.scope_sigs[idx];
        std::string leaf = sig.full_path;
        size_t last_slash = leaf.rfind('/');
        if (last_slash != std::string::npos) leaf = leaf.substr(last_slash + 1);

        ffrVCTrvsHdl hdl = obj->ffrCreateVCTraverseHandle(sig.idcode);
        if (!hdl) continue;

        if (hdl->ffrHasIncoreVC()) {
            fsdbTag64 time;
            byte_T *vc_ptr;
            int count = 0;
            hdl->ffrGetMinXTag(&time);
            hdl->ffrGotoXTag(&time);

            printf("  %s:\n", leaf.c_str());
            do {
                hdl->ffrGetXTag(&time);
                if (FSDB_RC_SUCCESS == hdl->ffrGetVC(&vc_ptr)) {
                    std::string val = format_vc(hdl, vc_ptr);
                    std::string hex = bin_to_hex(val);
                    uint64_t raw_t = ((uint64_t)time.H << 32) | time.L;
                    std::string t_ns = format_time_ns(raw_t);
                    printf("    t=%-12s  %s (0x%s)\n", t_ns.c_str(), val.c_str(), hex.c_str());
                    count++;
                    if (count >= g_debug_trans) {
                        printf("    ... (capped at %d transitions)\n", g_debug_trans);
                        break;
                    }
                }
            } while (FSDB_RC_SUCCESS == hdl->ffrGotoNextVC());

            if (count == 0) printf("    (no transitions in window)\n");
        }

        hdl->ffrFree();
    }

    obj->ffrResetSignalList();
    obj->ffrUnloadSignals();

    fsdbXTag reset_start, reset_close;
    reset_start.hltag.H = 0; reset_start.hltag.L = 0;
    reset_close.hltag.H = 0xFFFFFFFF; reset_close.hltag.L = 0xFFFFFFFF;
    obj->ffrResetViewWindow(&reset_start, &reset_close);
}

static void cmd_batch(ffrObject *obj, const char *fsdb_name, FILE *input) {
    char line[4096];
    int query_num = 0;

    while (fgets(line, sizeof(line), input)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        std::vector<std::string> tokens;
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            std::string tok;
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') tok += *p++;
                if (*p == '\'') p++;
            } else {
                while (*p && *p != ' ' && *p != '\t') tok += *p++;
            }
            tokens.push_back(tok);
        }
        if (tokens.empty()) continue;

        query_num++;
        printf("=== Query %d: %s ===\n", query_num, line);

        const char *cmd = tokens[0].c_str();

        if (strcmp(cmd, "info") == 0) {
            cmd_info(obj, fsdb_name);
        }
        else if (strcmp(cmd, "scopes") == 0) {
            const char *keyword = (tokens.size() > 1) ? tokens[1].c_str() : NULL;
            cmd_scopes(obj, keyword);
        }
        else if (strcmp(cmd, "signals") == 0) {
            const char *pattern = NULL;
            int max_results = g_signals_max;
            for (size_t i = 1; i < tokens.size(); i++) {
                if (tokens[i] == "-n" && i+1 < tokens.size()) {
                    max_results = atoi(tokens[++i].c_str());
                } else {
                    pattern = tokens[i].c_str();
                }
            }
            cmd_signals(obj, pattern, max_results);
        }
        else if (strcmp(cmd, "value") == 0) {
            if (tokens.size() < 3) {
                fprintf(stderr, "  Usage: value <signal> <time>\n");
            } else {
                cmd_value(obj, tokens[1].c_str(), tokens[2].c_str());
            }
        }
        else if (strcmp(cmd, "changes") == 0) {
            if (tokens.size() < 2) {
                fprintf(stderr, "  Usage: changes <signal> [begin] [end] [-n max]\n");
            } else {
                const char *begin_str = NULL;
                const char *end_str = NULL;
                int max_changes = g_changes_max;
                for (size_t i = 2; i < tokens.size(); i++) {
                    if (tokens[i] == "-n" && i+1 < tokens.size()) {
                        max_changes = atoi(tokens[++i].c_str());
                    } else if (!begin_str) {
                        begin_str = tokens[i].c_str();
                    } else {
                        end_str = tokens[i].c_str();
                    }
                }
                cmd_changes(obj, tokens[1].c_str(), begin_str, end_str, max_changes);
            }
        }
        else if (strcmp(cmd, "freq") == 0) {
            if (tokens.size() < 2) {
                fprintf(stderr, "  Usage: freq <block_name>\n");
            } else {
                cmd_freq(obj, tokens[1].c_str());
            }
        }
        else if (strcmp(cmd, "debug") == 0) {
            if (tokens.size() < 3) {
                fprintf(stderr, "  Usage: debug <block_name> <time> [-w ns]\n");
            } else {
                uint64_t window_ns = 20;
                for (size_t i = 3; i < tokens.size(); i++) {
                    if (tokens[i] == "-w" && i+1 < tokens.size()) {
                        window_ns = (uint64_t)atol(tokens[++i].c_str());
                    }
                }
                cmd_debug(obj, tokens[1].c_str(), tokens[2].c_str(), window_ns);
            }
        }
        else {
            fprintf(stderr, "  Unknown command: %s\n", cmd);
        }
        printf("\n");
    }
    printf("Batch complete: %d queries\n", query_num);
}

// ---- Usage ----

static void usage(const char *prog) {
    printf("Usage: %s <fsdb_file> <command> [args...]\n\n", prog);
    printf("Commands:\n");
    printf("  info                              Show FSDB file information\n");
    printf("  scopes <keyword>                  List scopes matching keyword (shallowest first)\n");
    printf("  signals [pattern] [-n max]        List signals (glob filter, default max 100)\n");
    printf("  value <signal> <time>             Get signal value at time\n");
    printf("  changes <signal> [begin] [end] [-n max]  Show value changes (default max 1000)\n");
    printf("  freq <block_name>                 Find block, detect clocks, measure frequency\n");
    printf("  debug <block_name> <time> [-w ns] Dump key signals around failure time (default ±20ns)\n");
    printf("  batch [file]                      Run multiple queries from file or stdin\n");
    printf("\nPatterns use shell glob syntax: * matches anything, ? matches one char\n");
    printf("Bare names auto-match as substrings. 'scope signal' does scoped search.\n");
    printf("\nExamples:\n");
    printf("  %s waves.fsdb info\n", prog);
    printf("  %s waves.fsdb scopes core\n", prog);
    printf("  %s waves.fsdb signals '*clk*'\n", prog);
    printf("  %s waves.fsdb signals '*dut*data*' -n 50\n", prog);
    printf("  %s waves.fsdb value /top/dut/clk 1000\n", prog);
    printf("  %s waves.fsdb value 'core clk' 1000\n", prog);
    printf("  %s waves.fsdb changes /top/dut/data 500 2000\n", prog);
    printf("  %s waves.fsdb freq core\n", prog);
    printf("  %s waves.fsdb debug core_top 5213000000\n", prog);
    printf("  %s waves.fsdb debug core_top 5213000000 -w 50\n", prog);
    printf("  %s waves.fsdb batch queries.txt\n", prog);
}

// ---- Main ----

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *fsdb_name = argv[1];
    const char *command = argv[2];

    if (FALSE == ffrObject::ffrIsFSDB(const_cast<char*>(fsdb_name))) {
        fprintf(stderr, "Error: %s is not a valid FSDB file\n", fsdb_name);
        return 1;
    }

    ffrObject *obj = ffrObject::ffrOpen3(const_cast<char*>(fsdb_name));
    if (!obj) {
        fprintf(stderr, "Error: Failed to open %s\n", fsdb_name);
        return 1;
    }

    obj->ffrSetTreeCBFunc(tree_cb, NULL);
    init_config();
    init_time_scale(obj);
    init_sim_range(obj);
    g_ctx.mode = CB_MODE_NONE;

    if (strcmp(command, "info") == 0) {
        cmd_info(obj, fsdb_name);
    }
    else if (strcmp(command, "scopes") == 0) {
        const char *keyword = (argc > 3) ? argv[3] : NULL;
        cmd_scopes(obj, keyword);
    }
    else if (strcmp(command, "signals") == 0) {
        const char *pattern = NULL;
        int max_results = g_signals_max;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
                max_results = atoi(argv[++i]);
            } else {
                pattern = argv[i];
            }
        }
        cmd_signals(obj, pattern, max_results);
    }
    else if (strcmp(command, "value") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s <fsdb> value <signal> <time>\n", argv[0]);
            obj->ffrClose();
            return 1;
        }
        cmd_value(obj, argv[3], argv[4]);
    }
    else if (strcmp(command, "changes") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s <fsdb> changes <signal> [begin] [end] [-n max]\n", argv[0]);
            obj->ffrClose();
            return 1;
        }
        const char *begin_str = NULL;
        const char *end_str = NULL;
        int max_changes = g_changes_max;
        const char *sig = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
                max_changes = atoi(argv[++i]);
            } else if (!begin_str) {
                begin_str = argv[i];
            } else {
                end_str = argv[i];
            }
        }
        cmd_changes(obj, sig, begin_str, end_str, max_changes);
    }
    else if (strcmp(command, "freq") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s <fsdb> freq <block_name>\n", argv[0]);
            obj->ffrClose();
            return 1;
        }
        cmd_freq(obj, argv[3]);
    }
    else if (strcmp(command, "debug") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s <fsdb> debug <block_name> <time> [-w window_ns]\n", argv[0]);
            obj->ffrClose();
            return 1;
        }
        uint64_t window_ns = 20;
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "-w") == 0 && i+1 < argc) {
                window_ns = (uint64_t)atol(argv[++i]);
            }
        }
        cmd_debug(obj, argv[3], argv[4], window_ns);
    }
    else if (strcmp(command, "batch") == 0) {
        FILE *input = stdin;
        if (argc > 3) {
            input = fopen(argv[3], "r");
            if (!input) {
                fprintf(stderr, "Error: Cannot open batch file %s\n", argv[3]);
                obj->ffrClose();
                return 1;
            }
        }
        cmd_batch(obj, fsdb_name, input);
        if (input != stdin) fclose(input);
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", command);
        usage(argv[0]);
        obj->ffrClose();
        return 1;
    }

    obj->ffrClose();
    return g_exit_code;
}
