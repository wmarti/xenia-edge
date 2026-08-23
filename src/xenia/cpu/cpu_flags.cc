/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/cpu_flags.h"

DEFINE_string(cpu, "any", "Does nothing. CPU backend [any, x64].", "CPU");

DEFINE_string(
    load_module_map, "",
    "Loads a .map for symbol names and to diff with the generated symbol "
    "database.",
    "CPU");

DEFINE_bool(accurate_vmx_denormal_flush, false,
            "Flush denormals in the VMX multiply-add and dot product opcodes "
            "even while the guest has NJM cleared, which is what hardware "
            "does. Costs performance in vector heavy code and only matters to "
            "a title that clears VSCR.NJ through mtvscr.",
            "CPU");

DEFINE_bool(disassemble_functions, false,
            "Disassemble functions during generation.", "CPU");

DEFINE_bool(trace_function_coverage, false,
            "Count how many times each guest instruction executes and report "
            "the totals in the guestcoverage section of the profiler dump.",
            "CPU");

DEFINE_uint32(
    cpu_trace_mask, 0,
    "JIT execution trace modes to log (bitmask): 1=instructions, 2=data, "
    "4=function calls (7=all). Each mode must be compiled in to be usable.",
    "CPU");

DEFINE_bool(validate_hir, false,
            "Perform validation checks on the HIR during compilation.", "CPU");

// https://github.com/bitsh1ft3r/Xenon/blob/091e8cd4dc4a7c697b4979eb200be7c9dee3590b/Xenon/Core/XCPU/PPU/PowerPC.h#L370
DEFINE_uint64(
    pvr, 0x710700,
    "Known PVR's.\n"
    " 0x710200 = Used by Zephyr \n"
    " 0x710300 = Used by Zephyr\n"
    " 0x710500 = Used by Jasper\n"
    " 0x710700 = Default\n"
    " 0x710800 = Used by Corona V1 & V2\n"
    "Processor version and revision number.\nBits 0 to 15 are the version "
    "number.\nBits 16 to 31 are the revision number.\nNote: Some XEXs (such as "
    "mfgbootlauncher.xex) may check for a value that's less than 0x710700.",
    "CPU");

// Breakpoints:
DEFINE_uint64(break_on_instruction, 0,
              "int3 before the given guest address is executed.", "CPU");
DEFINE_int32(break_condition_gpr, -1, "GPR compared to", "CPU");
DEFINE_uint64(break_condition_value, 0, "value compared against", "CPU");
DEFINE_string(break_condition_op, "eq", "comparison operator", "CPU");
DEFINE_bool(break_condition_truncate, true, "truncate value to 32-bits", "CPU");

DEFINE_bool(break_on_debugbreak, true, "int3 on JITed __debugbreak requests.",
            "CPU");

// Guest spin-wait release. See DELAY_EXECUTION in a64_seq_memory.cc.
DEFINE_uint32(
    db16cyc_yield_after, 2,
    "After this many consecutive guest db16cyc spin-wait delays on a thread, "
    "release the core instead of only stalling in it. 0 disables escalation.\n"
    "A guest wait loop built from db16cyc occupies a core for the whole wait "
    "no "
    "matter which delay instruction it lowers to, because the wait's length is "
    "set by the event being waited for, not by the delay. Apple's CPU "
    "Optimization Guide 4.0 section 7.3 recommends blocking over spinning for "
    "exactly this reason: a spin-wait on a P core pushes other work onto the E "
    "cores and denies the system otherwise idle time.\n"
    "Counted per EMITTED delay, and consecutive delays are coalesced into one, "
    "so a guest sled of eight db16cyc advances this by one per loop iteration "
    "rather than by eight.",
    "CPU");

DEFINE_uint32(db16cyc_sleep_ns, 60000,
              "Nanoseconds to sleep when db16cyc_yield_after trips. A plain "
              "sched_yield is not enough: it returns immediately when other "
              "cores are idle, leaving the thread spinning.",
              "CPU");

DEFINE_uint32(
    db16cyc_consecutive_gap_ns, 1000,
    "Two db16cyc executions further apart than this do not count as the same "
    "wait loop, and the consecutive counter restarts. Pure wait-loop "
    "iterations are ~0.1us apart; sparse use of db16cyc between real work is "
    "much further apart and must never accumulate to a release.",
    "CPU");

DEFINE_path(
    jit_perf_map, "",
    "Writes a JIT symbol map to the given file: one '<host_hex> <size_hex> "
    "<name>' line per translated guest function. Lets a host profiler "
    "attribute "
    "raw JIT sample addresses to guest functions - guest code has no symbols, "
    "so "
    "samples otherwise land on bare addresses and a time profile cannot be "
    "checked against a codegen ranking at all.",
    "CPU");
