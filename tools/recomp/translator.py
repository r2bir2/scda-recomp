"""
Function-level x86 → C translator.

For each function:
1. Read raw bytes from XBE
2. Disassemble with Capstone
3. Build basic blocks
4. Lift each block to C statements
5. Generate a complete C function

Produces compilable C code using recomp_types.h macros.
"""

import bisect
import json
import glob
import os
import struct

# Import the functions, not the VA constants: configure_from_xbe() rebinds those
# at startup, so a by-value import would freeze the fallback layout.
from .config import va_to_file_offset, is_code_address
from . import config as _config
from .disasm import Disassembler
from .lifter import (Lifter, lift_basic_block, detect_seh_helpers,
                     detect_setjmp_helpers, _operand_width, FLAG_SAVE_VARS,
                     FP_CMP_SETTERS)


def _snapshot_width(ops):
    width = _operand_width(ops[0]) or _operand_width(ops[1])
    return width if width in (1, 2, 4) else 4


def _flag_states_agree(a, b):
    """Whether two predecessors hand a shared successor the same flags.

    A cmp/test snapshots its operands into the function-wide _fa/_fb/_fas/_fbs
    where it executes, and every condition built from such a state reads only
    those snapshots plus the compare's width (js/jns). Two predecessors ending
    in `cmp [ebx+8], edi` and `cmp ecx, edi` therefore give a shared successor
    exactly the same condition, even though their operands differ. Comparing
    the raw operands instead dropped such merges to the never-assigned _flags
    fallback -- an always-false branch (SCDA VerifyImport at 0x7EB4D, Update
    320). Every other flag setter still has to match exactly.
    """
    if a is None or b is None:
        return False
    # Float compares (fcomi/comiss families) all snapshot into g_fp_cmp, so
    # any two of them hand a join the same conditions. Requiring an exact
    # match dropped e.g. SCDA's `comiss xmm1,[ebx+0x764]` / `comiss xmm2,xmm1`
    # join at 0x0008C238 to the always-false _flags (docs Update 354).
    if a[0] in FP_CMP_SETTERS and b[0] in FP_CMP_SETTERS:
        return True
    if (a[0] in ("cmp", "test") and a[0] == b[0]
            and len(a[1]) >= 2 and len(b[1]) >= 2):
        return _snapshot_width(a[1]) == _snapshot_width(b[1])
    return a == b


def _merge_flag_states(a, b):
    """The flag state a join inherits from two predecessors, or None.

    Agreeing states pass through (_flag_states_agree). Differing cmp/test
    setters -- a 32-bit `cmp eax, -1` and an 8-bit `test al, dl` feeding the
    same `setne` in SCDA's portal walk sub_002AD5AF -- still share ZF: both
    record the result it comes from in _fzr, so the join gets the "zres"
    state, which resolves je/jne/sete/setne/cmove/cmovne (docs Update 353).
    """
    if _flag_states_agree(a, b):
        return a
    zero_kinds = ("cmp", "test", "zres")
    if a is not None and b is not None and a[0] in zero_kinds and b[0] in zero_kinds:
        return ("zres", [])
    # Result-based setters writing the same destination (`and ebx, m` and
    # `inc ebx` feeding one `je`, SCDA 0x0030A4F5) agree on ZF/SF, which
    # both read from that destination: the "zsres" state (docs Update 354).
    if (a is not None and b is not None
            and a[0] in _RESULT_ZS_SETTERS and b[0] in _RESULT_ZS_SETTERS
            and a[1] and b[1] and a[1][0] == b[1][0]):
        return ("zsres", [a[1][0]])
    return None

def _block_incoming_state(bb, preds, start, states, optimistic):
    """Merge of the predecessors' out-states (None if any is unknown, unless
    optimistic, where unknown predecessors are ignored)."""
    sources = preds[bb.start]
    if bb.start == start or not sources:
        return None
    known = [states[p] for p in sources if p in states]
    if not known or (len(known) < len(sources) and not optimistic):
        return None
    merged = known[0]
    for other in known[1:]:
        merged = _merge_flag_states(merged, other)
        if merged is None:
            break
    return merged


def _solve_block_flag_states(blocks, preds, start, lift_out, max_rounds=16):
    """The flag state each block inherits, solved to a fixpoint.

    A single address-order walk cannot see a back edge's state, so a loop
    head whose jcc consumes a `cmp` made both before the loop and at its
    bottom fell to the never-assigned _flags -- an always-false branch. zlib's
    pqdownheap in SCDA (sub_0004ED7F, `jge` at 0x4EDA2) then ran past the
    heap end and gen_bitlen wrote through a wild index into the NV2A
    aperture (docs Update 355). Starting optimistic (a predecessor not lifted
    yet is ignored) and re-lifting until nothing changes gives the greatest
    fixpoint; _merge_flag_states only moves a state down, so it converges.
    It is exact for flags: around a loop that sets none, the back edge really
    carries the entry's flags.

    lift_out(bb, incoming) returns the block's out-state. Returns
    {block start: incoming state}, or None if no fixpoint was reached.
    """
    states = {}
    for _ in range(max_rounds):
        changed = False
        for bb in blocks:
            out = lift_out(bb, _block_incoming_state(bb, preds, start, states,
                                                     optimistic=True))
            if bb.start not in states or states[bb.start] != out:
                states[bb.start] = out
                changed = True
        if not changed:
            return {bb.start: _block_incoming_state(bb, preds, start, states,
                                                    optimistic=False)
                    for bb in blocks}
    return None


# Setters whose ZF/SF are those of the value left in their first operand.
# Shifts are excluded: a zero count leaves the flags untouched.
_RESULT_ZS_SETTERS = frozenset({
    "add", "sub", "and", "or", "xor", "inc", "dec", "neg", "adc", "sbb",
    "zsres",
})


def _fixup_icall_esp_save(lines):
    """
    Post-process generated C lines to insert _icall_esp save points.

    When RECOMP_ICALL_SAFE is used, we need to save g_esp BEFORE any
    args are pushed so the macro can restore it on lookup failure.

    Scans backwards from each RECOMP_ICALL_SAFE line to find consecutive
    PUSH32 lines (the arg pushes), then inserts a save before the first.

    A push of a callee-saved register can be either the function saving it or
    an argument that happens to live in it, and the two need telling apart:
    absorbing a save makes the failure path rewind g_esp over the function's
    own frame, and the epilogue then pops its registers from too high -- silent
    caller corruption. Leaving an argument behind is the mirror image, and
    shifts the epilogue the other way.

    Push and pop counts separate them. A register popped at least as often as
    it is pushed is restored on every path out, so a push of it is a save and
    the argument run ends there -- "at least", not "exactly", because a
    function with several epilogues pops once per return path. A register
    pushed more often than it is popped has a push nobody restores -- an
    argument -- and the run absorbs it as before.

    Where that is still ambiguous, stopping early is the safer error: it
    under-rewinds, which surfaces as the detectable "epilogue never ran" leak,
    rather than as a caller silently carrying a wrong register.
    """
    import re
    saves = tuple(
        "PUSH32(esp, %s)" % reg
        for reg in ("ebx", "esi", "edi", "ebp")
        if 0 < sum("PUSH32(esp, %s)" % reg in line for line in lines)
        <= sum("POP32(esp, %s)" % reg in line for line in lines)
    )
    result = []

    # The first basic block can contain the native function prologue as well
    # as an indirect call.  A backwards scan that treats the prologue's saved
    # registers as call arguments restores ESP all the way to function entry
    # when lookup fails.  That discards the frame and makes later pushes
    # overwrite the caller's locals.  Find the end of the conventional
    # prologue so first-block ICALL saves can never move above it.
    entry_label_idx = None
    next_label_idx = len(lines)
    for i, line in enumerate(lines):
        if re.match(r'^loc_[0-9A-Fa-f]+:', line.strip()):
            if entry_label_idx is None:
                entry_label_idx = i
            else:
                next_label_idx = i
                break

    prologue_end_idx = 0
    if entry_label_idx is not None:
        i = entry_label_idx + 1
        if i < len(lines) and lines[i].strip().startswith('RECOMP_TRACE_PC('):
            i += 1

        stripped = lines[i].strip() if i < len(lines) else ''
        if stripped == 'PUSH32(esp, ebp);':
            i += 1
            if i < len(lines) and lines[i].strip() == 'ebp = esp;':
                i += 1
                # _lift_mov() unconditionally follows "ebp = esp;" with these
                # two frame-publish lines (see lifter.py) so a frameless
                # callee reached through g_ebp/g_seh_ebp sees this frame, not
                # stale data from whoever called last. Every standard-prologue
                # function has them, so skipping past them here is not
                # optional: without it, this scan stops one line short of
                # "esp = esp - N;" below, prologue_end_idx lands on the first
                # of these two lines instead of after the callee-saved-
                # register pushes that follow, and the icall-save clamp a few
                # lines down (`first_push_idx = max(first_push_idx,
                # prologue_end_idx)`) no longer reaches far enough to stop a
                # backward scan that has already walked past those pushes --
                # exactly the case the push/pop *count* heuristic above this
                # function also misses whenever a callee-saved register is
                # separately reused as a repeated zero/default argument value
                # elsewhere in the same function (pushed far more often than
                # it is popped, so it never qualifies as a "save" by count
                # alone). Found via sub_00041F56: esi pushed 14x/popped 1x
                # (13 of those pushes are "push 0" argument idiom, not
                # saves), so the count heuristic missed it entirely and the
                # positional clamp was the only thing that could have caught
                # it -- until it landed two lines too early. See
                # docs/SCDA_WORKLOG.md, "continued 29".
                if (i < len(lines) and lines[i].strip() ==
                        'g_ebp = ebp; /* publish frame for frameless callees */'):
                    i += 1
                if i < len(lines) and lines[i].strip() == 'g_seh_ebp = ebp;':
                    i += 1
            if (i < len(lines) and
                    re.match(r'^esp = esp - (?:0x[0-9A-Fa-f]+|[0-9]+);$',
                             lines[i].strip())):
                i += 1

        # FPO functions commonly start by saving one or more callee-saved
        # registers without establishing EBP. A handful of pure setup lines
        # -- register/memory loads with no stack or control-flow effect,
        # e.g. a vtable fetch ("ecx = MEM32(g); eax = MEM32(ecx);") -- can
        # precede those saves without being part of them. Skip past any such
        # line before looking for the PUSH32 run: without this, the scan
        # below stops on the first non-push line it sees, landing
        # prologue_end_idx one or more lines short of the real saves and
        # leaving the icall-save clamp too weak to stop the backward scan
        # from walking past them.
        #
        # Found via sub_0026940F: two vtable-fetch lines precede "PUSH32(esp,
        # esi); PUSH32(esp, edi);", and both esi and edi are separately reused
        # as a repeated argument value across nine later indirect-call sites
        # in the same function (each pushed far more often than it is popped),
        # so the push/pop *count* heuristic above this function also misses
        # them -- this positional skip is the only remaining thing that can
        # recognize the save. Without it, an unresolved indirect call whose
        # failure path rewinds g_esp to a save captured before these pushes
        # discards them permanently, and the function's own epilogue then
        # restores esi/edi (and everything the caller does afterward, off the
        # same now-8-bytes-too-high stack pointer) from the wrong slot. See
        # docs/SCDA_WORKLOG.md, Update 232.
        while i < len(lines):
            stripped = lines[i].strip()
            if re.match(r'^PUSH32\(esp, (ebx|esi|edi)\);$', stripped):
                break
            # A flag-operand copy (lifter._materialize_flag_ops) is not part
            # of the frame setup and must not end the prologue here.
            if stripped.startswith('_fsv'):
                i += 1
                continue
            if (not stripped or
                    'esp' in stripped or
                    'RECOMP_' in stripped or
                    'PUSH32' in stripped or
                    'POP32' in stripped or
                    'goto ' in stripped or
                    'return;' in stripped or
                    stripped.startswith('if (') or
                    re.match(r'^loc_[0-9A-Fa-f]+:', stripped)):
                break
            i += 1

        # FPO functions commonly start by saving one or more callee-saved
        # registers without establishing EBP.  Only accept a push as a save
        # when the generated function also contains the matching pop.
        while i < len(lines):
            if lines[i].strip().startswith('_fsv'):
                i += 1
                continue
            match = re.match(r'^PUSH32\(esp, (ebx|esi|edi)\);$',
                             lines[i].strip())
            if not match:
                break
            reg = match.group(1)
            if not any(f'POP32(esp, {reg});' in later for later in lines[i + 1:]):
                break
            i += 1
        prologue_end_idx = i
    # Find indices of all ICALL_SAFE lines
    icall_indices = []
    for i, line in enumerate(lines):
        if 'RECOMP_ICALL_SAFE(' in line:
            icall_indices.append(i)

    if not icall_indices:
        return lines  # nothing to do

    # For each ICALL, determine where to insert the save
    insert_before = set()  # map: line_index → True (insert save before this line)
    for icall_idx in icall_indices:
        # The ICALL line itself is "PUSH32(esp, <retva>); RECOMP_ICALL_SAFE(...)"
        # Look backwards for consecutive lines containing PUSH32(esp,
        first_push_idx = icall_idx
        j = icall_idx - 1
        while j >= 0:
            stripped = lines[j].strip()
            # Skip blank lines
            if not stripped:
                j -= 1
                continue
            # A completed direct call ends the argument run, and must be tested
            # before the generic PUSH32 check below: a direct call is emitted as
            # "PUSH32(esp, <retva>); name();" on one line, so it also looks like
            # an argument push. Treating it as one put the _icall_esp save
            # *before* the direct call, and an ICALL that then failed rewound
            # g_esp over a call that had already returned.
            if '/* call 0x' in stripped:
                break
            # A saved callee-saved register belongs to this function's frame,
            # not to the call's arguments; the run ends here.
            if any(stripped.startswith(save) for save in saves):
                break
            # Check if this is a PUSH32 line (arg push)
            if stripped.startswith('PUSH32(esp,'):
                first_push_idx = j
                j -= 1
                continue
            # Check if this is a non-push instruction that could be part of
            # arg evaluation (e.g., "eax = MEM32(...);") - these are interleaved
            # with pushes in the x86 code. We need to look past them.
            # Stop at labels, gotos, other control flow, or other ICALL lines.
            if (re.match(r'^loc_[0-9A-Fa-f]+:', stripped) or
                'goto ' in stripped or
                'RECOMP_ICALL' in stripped or
                'return;' in stripped or
                stripped.startswith('if (') or
                stripped.startswith('POP32(')):
                break
            # It's an interleaved computation - skip past it
            j -= 1
            continue

        if icall_idx < next_label_idx:
            first_push_idx = max(first_push_idx, prologue_end_idx)
        insert_before.add(first_push_idx)

    # Build result with saves inserted
    for i, line in enumerate(lines):
        if i in insert_before:
            # Determine indentation from the current line
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}{{ uint32_t _icall_esp = g_esp;")
        result.append(line)
        if 'RECOMP_ICALL_SAFE(' in line:
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}}}")

    return result


# The x87 stack accessors the lifter's output is written against. Module-level
# so tools/conformance emits byte-identical macros: a harness with its own copy
# would still pass after these changed, which is the dangerous direction.
FP_STACK_MACROS = [
    "    #define fp_push(v) do { double _fp_value = (v); \\",
    "        g_fp_top = (g_fp_top + 7u) & 7u; \\",
    "        g_fp_stack[g_fp_top] = _fp_value; } while (0)",
    "    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)",
    "    #define fp_top() g_fp_stack[g_fp_top]",
    "    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]",
    "    #define fp_st1() fp_st(1)",
]

FP_STACK_UNDEFS = [
    "    #undef fp_push",
    "    #undef fp_pop",
    "    #undef fp_top",
    "    #undef fp_st",
    "    #undef fp_st1",
]



def xbe_title(xbe_data, xbe_path):
    """Human-readable title for generated-file banners.

    Read from the XBE certificate so the output names the game it came from.
    These banners used to be hardcoded to "Burnout 3: Takedown" -- the toolkit
    grew out of that title -- so every other game's generated C claimed to be
    Burnout 3. Falls back to the file's basename if the certificate cannot be
    read; a banner is cosmetic and must never fail a build.
    """
    try:
        base = struct.unpack_from("<I", xbe_data, 0x104)[0]
        cert_va = struct.unpack_from("<I", xbe_data, 0x118)[0]
        off = cert_va - base + 0x0C
        name = xbe_data[off:off + 80].decode("utf-16-le").partition(chr(0))[0].strip()
        if name:
            return name
    except Exception:
        pass
    return os.path.splitext(os.path.basename(xbe_path))[0]


class FunctionTranslator:
    """Translates individual x86 functions to C source code."""

    def __init__(self, xbe_data, func_db, label_db=None, classification_db=None,
                 abi_db=None, seh_prolog=None, seh_epilog=None,
                 setjmp_fn=None, longjmp_fn=None,
                 trace_functions=None):
        """
        xbe_data: bytes - raw XBE file contents
        func_db: dict - addr → function info from functions.json
        label_db: dict - addr → name from labels.json
        classification_db: dict - addr → classification from identified_functions.json
        abi_db: dict - addr → ABI info from abi_functions.json
        seh_prolog/seh_epilog: override the detected SEH helper addresses
        """
        self.xbe_data = xbe_data
        self.func_db = func_db
        self.label_db = label_db or {}
        self.classification_db = classification_db or {}
        self.abi_db = abi_db or {}
        self.trace_functions = set(trace_functions or ())
        # Functions whose memory accesses take IOR/IOW (see translate_function)
        self.io_functions = set()
        self.disasm = Disassembler()
        self.lifter = Lifter(func_db=func_db, label_db=label_db, abi_db=abi_db,
                             xbe_data=xbe_data, seh_prolog=seh_prolog,
                             setjmp_fn=setjmp_fn, longjmp_fn=longjmp_fn,
                             seh_epilog=seh_epilog)
        self.owned_function_starts = set()
        self.recovered_function_starts = set()
        self._recovered_cfg = {}
        self._ownership_ready = False

    def discover_static_indirect_targets(self):
        """Recover function entries from bounded static callback tables."""
        original_starts = sorted(self.func_db)
        recovered_callers = {}

        for caller, func_info in list(self.func_db.items()):
            end = func_info.get("end", caller)
            raw_bytes = self._read_func_bytes(caller, end)
            if not raw_bytes:
                continue
            instructions = self.disasm.disassemble_function(
                raw_bytes, caller, end)
            for lower, upper in self._find_static_indirect_ranges(instructions):
                targets = self._read_static_callback_table(
                    lower, upper, original_starts)
                if targets is None:
                    continue
                for target in targets:
                    if target not in self.func_db:
                        recovered_callers.setdefault(target, set()).add(caller)

        for target, callers in sorted(recovered_callers.items()):
            next_index = bisect.bisect_right(original_starts, target)
            if next_index == 0 or next_index >= len(original_starts):
                continue
            previous = self.func_db[original_starts[next_index - 1]]
            next_start = original_starts[next_index]
            next_func = self.func_db[next_start]
            if previous.get("end", previous["_addr"]) > target:
                continue
            section = next_func.get("section", "")
            if section in (".rdata", ".data"):
                continue

            raw_bytes = self._read_func_bytes(target, next_start)
            if not raw_bytes:
                continue
            instructions = self.disasm.disassemble_function(
                raw_bytes, target, next_start)
            if not instructions or not any(insn.is_ret for insn in instructions):
                continue

            self.func_db[target] = {
                "_addr": target,
                "start": f"0x{target:08X}",
                "end": next_start,
                "size": next_start - target,
                "name": self.label_db.get(target, f"sub_{target:08X}"),
                "section": section,
                "confidence": 0.9,
                "detection_method": "static_indirect_table",
                "num_instructions": len(instructions),
                "has_prologue": self._func_has_prologue(instructions),
                "calls_to": [],
                "called_by": sorted(callers),
            }
            self.recovered_function_starts.add(target)

        return self.recovered_function_starts

    @staticmethod
    def _find_static_indirect_ranges(instructions, max_bytes=0x10000):
        """Find immediate-backed ranges in functions that call a register."""
        constants = {}
        ranges = set()
        has_indirect_call = False

        for insn in instructions:
            operands = insn.operands
            if (insn.mnemonic == "mov" and len(operands) >= 2
                    and operands[0].type == "reg"):
                destination = operands[0].reg
                source = operands[1]
                if source.type == "imm":
                    constants[destination] = source.imm
                elif source.type == "reg" and source.reg in constants:
                    constants[destination] = constants[source.reg]
                else:
                    constants.pop(destination, None)
            elif (insn.mnemonic == "cmp" and len(operands) >= 2
                    and operands[0].type == "reg"
                    and operands[1].type == "reg"):
                lower = constants.get(operands[0].reg)
                upper = constants.get(operands[1].reg)
                if (lower is not None and upper is not None
                        and lower < upper and lower % 4 == 0
                        and upper % 4 == 0 and upper - lower <= max_bytes):
                    ranges.add((lower, upper))
            elif (insn.is_call and insn.call_target is None
                    and operands and operands[0].type == "reg"):
                has_indirect_call = True

        return sorted(ranges) if has_indirect_call else []

    def _read_static_callback_table(self, lower, upper, original_starts):
        """Validate and return a bounded table of static code pointers."""
        targets = []
        for entry_va in range(lower, upper, 4):
            offset = va_to_file_offset(entry_va)
            if offset is None or offset + 4 > len(self.xbe_data):
                return None
            target = struct.unpack_from('<I', self.xbe_data, offset)[0]
            if target in (0, 0xFFFFFFFF):
                continue

            index = bisect.bisect_right(original_starts, target)
            if index and self.func_db[original_starts[index - 1]].get(
                    "end", original_starts[index - 1]) > target:
                targets.append(target)
                continue
            if index == 0 or index >= len(original_starts):
                return None
            previous = self.func_db[original_starts[index - 1]]
            following = self.func_db[original_starts[index]]
            if (previous.get("section") != following.get("section")
                    or following.get("section") in (".rdata", ".data")
                    or va_to_file_offset(target) is None):
                return None
            targets.append(target)

        return targets if targets else None

    @staticmethod
    def _is_strong_entry(func_info):
        """Return whether an entry has evidence independent of seed recovery."""
        return bool(func_info.get("has_prologue") or func_info.get("called_by"))

    def discover_cfg_ownership(self):
        """Reassign weak seeds reached through a split computed-jump CFG."""
        if self._ownership_ready:
            return
        self._ownership_ready = True

        by_section = {}
        weak_by_section = {}
        for addr, info in self.func_db.items():
            section = info.get("section", "")
            if self._is_strong_entry(info):
                by_section.setdefault(section, []).append(addr)
            else:
                weak_by_section.setdefault(section, []).append(addr)

        for section, strong_starts in by_section.items():
            strong_starts.sort()
            weak_starts = sorted(weak_by_section.get(section, []))
            for index, start in enumerate(strong_starts[:-1]):
                original_end = self.func_db[start].get("end", start)
                upper = strong_starts[index + 1]
                if original_end >= upper:
                    continue

                weak_index = bisect.bisect_left(weak_starts, original_end)
                if (weak_index >= len(weak_starts)
                        or weak_starts[weak_index] >= upper):
                    continue

                raw_prefix = self._read_func_bytes(start, original_end)
                if not raw_prefix:
                    continue
                prefix = self.disasm.disassemble_function(
                    raw_prefix, start, original_end)
                bridges = {
                    insn.jump_target
                    for insn in prefix
                    if (insn.is_cond_jump and insn.jump_target is not None
                        and original_end <= insn.jump_target < upper)
                }
                has_indexed_jump = any(
                    insn.is_jump and insn.jump_target is None
                    and insn.operands and insn.operands[0].type == "mem"
                    and insn.operands[0].mem_index
                    for insn in prefix)
                if not bridges or not has_indexed_jump:
                    continue

                stop_addresses = {
                    addr for addr in self.func_db if start < addr < upper
                }
                recovered = self._recover_cfg(
                    start, upper, bridges, stop_addresses)
                if recovered is None:
                    continue
                instructions, jump_tables, cfg_targets = recovered
                owned = {
                    addr for addr in weak_starts[weak_index:]
                    if addr < upper and addr in cfg_targets
                }
                if not owned:
                    continue

                self.owned_function_starts.update(owned)
                self._recovered_cfg[start] = {
                    "end": max(insn.end_address for insn in instructions),
                    "instructions": instructions,
                    "jump_tables": jump_tables,
                }

    def _recover_cfg(self, start, upper, bridge_targets, stop_addresses):
        """Decode direct CFG edges and local indexed-table destinations."""
        raw_bytes = self._read_func_bytes(start, upper)
        if not raw_bytes:
            return None

        entry_points = {start, *bridge_targets}
        jump_tables = {}
        while True:
            instructions = self.disasm.disassemble_cfg(
                raw_bytes, start, upper, entry_points,
                stop_addresses=stop_addresses)
            changed = False
            for insn in instructions:
                if not insn.is_jump or insn.jump_target is not None:
                    continue
                if not insn.operands or insn.operands[0].type != "mem":
                    continue
                operand = insn.operands[0]
                if not operand.mem_index or operand.mem_base:
                    continue
                table_va = operand.mem_disp
                if not (start <= table_va < upper):
                    continue
                targets = self._read_local_jump_table(table_va, start, upper)
                if not targets:
                    continue
                jump_tables[table_va] = targets
                for target in targets:
                    if target not in entry_points:
                        entry_points.add(target)
                        changed = True
            if not changed:
                cfg_targets = {
                    insn.jump_target
                    for insn in instructions
                    if insn.jump_target is not None
                    and start <= insn.jump_target < upper
                }
                for targets in jump_tables.values():
                    cfg_targets.update(targets)
                return instructions, jump_tables, cfg_targets

    def _stub_ret_bytes(self, addr, max_insns=48):
        """Bytes the code at `addr` would have popped beyond the return address.

        Returns the immediate of the first `ret N` reachable by walking
        straight-line from addr, or 0 if the walk finds a plain `ret`, runs into
        a call or an unconditional jump, or finds nothing at all.

        Conditional branches are walked through rather than followed: the block
        this is used on is a switch arm whose arms all share one epilogue, so
        the not-taken path reaches the same ret. A call or a jmp means control
        genuinely leaves, and guessing past that is how you get a wrong answer
        that looks right.
        """
        offset = va_to_file_offset(addr)
        if offset is None or not self.xbe_data:
            return 0
        window = self.xbe_data[offset:offset + max_insns * 8]
        if not window:
            return 0
        try:
            decoded = self.disasm._cs.disasm(window, addr)
        except Exception:
            return 0
        for count, insn in enumerate(decoded):
            if count >= max_insns:
                break
            mnemonic = insn.mnemonic.lower()
            if mnemonic in ("call", "jmp"):
                return 0
            if mnemonic in ("ret", "retn"):
                try:
                    return int(insn.op_str, 0) if insn.op_str else 0
                except ValueError:
                    return 0
        return 0

    def _read_local_jump_table(self, table_va, lower, upper,
                               max_entries=256):
        """Read the contiguous pointer cluster around an indexed-jump base."""
        def scan(step, first):
            targets = []
            for index in range(first, max_entries + first):
                entry_va = table_va + step * index * 4
                offset = va_to_file_offset(entry_va)
                if offset is None or offset + 4 > len(self.xbe_data):
                    break
                target = struct.unpack_from('<I', self.xbe_data, offset)[0]
                if not (lower <= target < upper):
                    break
                targets.append(target)
            return targets

        backward = scan(-1, 1)
        forward = scan(1, 0)
        if len(backward) + len(forward) < 2:
            return []
        backward.reverse()
        return backward + forward

    def _read_func_bytes(self, start_va, end_va):
        """Read raw bytes for a function from the XBE."""
        offset = va_to_file_offset(start_va)
        if offset is None:
            return None
        size = end_va - start_va
        if offset + size > len(self.xbe_data):
            return None
        return self.xbe_data[offset:offset + size]

    def _determine_calling_convention(self, func_info):
        """Guess calling convention from function properties."""
        name = func_info.get("name", "")
        # thiscall methods have ecx = this
        if "thiscall" in name or func_info.get("calling_convention") == "thiscall":
            return "thiscall"
        return "cdecl"

    _CARRY_CC = frozenset({
        "b", "nae", "c", "ae", "nb", "nc", "be", "na", "a", "nbe",
    })

    @staticmethod
    def _function_needs_cf(instructions):
        """True when something in the function reads CF.

        Ported from upstream sp00nznet/xboxrecomp commit 2bfb9e05a8
        ("lifter: read the carry flag on branches after arithmetic"); see
        docs/SCDA_WORKLOG.md, 2026-09-11. Mirrors the lifter's own
        CF_TRACKED rule so the two agree on which setter a branch reads --
        computing _cf beside every add in the image would cost a line per
        add across the whole binary, so only functions that actually
        consume it declare it.
        """
        from .lifter import (FLAG_SETTERS, CF_TRACKED,
                             _EFLAGS_SETTERS, _FLAGS_UNDEFINED)

        last_setter = None
        for insn in instructions:
            m = insn.mnemonic
            if m in ("adc", "sbb", "stc", "clc", "cmc", "rcl", "rcr"):
                return True
            cc = None
            if m.startswith("j") and len(m) > 1:
                cc = m[1:]
            elif m.startswith("set"):
                cc = m[3:]
            if (cc in FunctionTranslator._CARRY_CC
                    and last_setter in CF_TRACKED):
                return True
            if m in FLAG_SETTERS or m in _EFLAGS_SETTERS:
                last_setter = m
            elif m in _FLAGS_UNDEFINED:
                last_setter = None
        return False

    def _func_has_prologue(self, instructions):
        """Check if function starts with push ebp; mov ebp, esp."""
        if len(instructions) < 2:
            return False
        return (instructions[0].mnemonic == "push" and
                instructions[0].op_str == "ebp" and
                instructions[1].mnemonic == "mov" and
                instructions[1].op_str == "ebp, esp")

    def _func_owns_a_frame(self, instructions):
        """True when the function has a frame, however it got one.

        __SEH_prolog builds its caller's frame for it -- "lea ebp, [esp+0x10]"
        inside the helper, after stashing the old ebp in the new frame -- so a
        function that calls it owns a real frame without ever writing ebp
        itself. Judging only on "push ebp; mov ebp, esp" calls those frameless,
        and then the frame is never re-published across their calls: any
        callee with a frame overwrites g_seh_ebp on entry and nothing puts it
        back, so the next frameless callee inherits a dead frame.

        Half-Life 2 hits this on its __finally funclets, which are ordinary
        calls into a shared tail that reads the parent's locals through
        g_seh_ebp. One of them leaves a critical section via
        [[ebp-0x2c]+0x580]; with a stale frame that read a KeyValues string as
        a pointer.
        """
        if self._func_has_prologue(instructions):
            return True
        seh_prolog = getattr(self.lifter, "SEH_PROLOG", None)
        if seh_prolog is None:
            return False
        return any(getattr(insn, "call_target", None) == seh_prolog
                   for insn in instructions)

    def translate_function(self, func_addr, func_info):
        """
        Translate a single function to C code.
        Returns a string of C source code, or None on failure.
        """
        start = func_addr
        recovered = self._recovered_cfg.get(start)
        end = recovered["end"] if recovered else func_info.get("end")
        if not end:
            end = start + func_info.get("size", 0)
        if end <= start:
            return None

        name = func_info.get("name", f"sub_{start:08X}")
        size = end - start

        # Read bytes from XBE
        raw_bytes = self._read_func_bytes(start, end)
        if not raw_bytes:
            return None

        # Set function bounds for the lifter
        self.lifter.func_start = start
        self.lifter.func_end = end
        self.lifter.jump_table_targets = (
            recovered["jump_tables"] if recovered else {})

        # Disassemble
        instructions = (recovered["instructions"] if recovered else
                        self.disasm.disassemble_function(raw_bytes, start, end))
        if not instructions:
            return None

        # Collect switch table targets as extra block leaders
        def collect_switch_leaders(insns):
            found = set()
            for insn in insns:
                if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                    for t in self.lifter._analyze_switch_table(insn.operands):
                        if start <= t < end:
                            found.add(t)
            return found

        switch_leaders = collect_switch_leaders(instructions)

        # A switch target the decode never produced an instruction for cannot
        # become a block leader, so it gets no label and its `goto` is dropped
        # as dead code -- the case then falls through to an unresolved indirect
        # branch. That happens whenever the jump table sits in .text ahead of
        # the code it points at: decoding the table as instructions leaves the
        # stream misaligned across the first case. Re-decode, telling the
        # disassembler where the real instruction boundaries are.
        #
        # Repeat until nothing new turns up: a case body decoded only by the
        # resync can hold a dispatch of its own. MSVC's memcpy is exactly that
        # -- its tail-copy dispatches (0x00367436, 0x003674E2) sit in cases of
        # the first switch, so one round never saw them and every tail copy
        # became an unresolved branch (docs Update 333).
        if recovered is None:
            resync = set()
            for _round in range(8):
                missing = switch_leaders - {insn.address for insn in instructions}
                if not missing:
                    break
                resync |= missing
                instructions = self.disasm.disassemble_function(
                    raw_bytes, start, end, resync=resync)
                switch_leaders |= collect_switch_leaders(instructions)

        # Build basic blocks
        blocks = self.disasm.build_basic_blocks(
            instructions, start, end,
            extra_leaders=switch_leaders if switch_leaders else None)
        if not blocks:
            return None

        # A function that names an APU or NV2A register by constant address
        # is a device driver routine (DirectSound, D3D): its memory accesses
        # go through IOR/IOW so register accesses skip the fault handler, the
        # ones through a computed pointer included. Driver routines that only
        # ever hold a pointer to the registers come from --io-functions
        # (docs Update 643).
        self.lifter.io_function = start in self.io_functions or any(
            op.type == "mem" and not op.mem_base and not op.mem_index
            and getattr(op, "mem_segment", None) != "fs"
            and 0xFD000000 <= (op.mem_disp & 0xFFFFFFFF) < 0xFE880000
            for insn in instructions for op in (insn.operands or ()))

        # Get classification and ABI info
        cls_info = self.classification_db.get(start, {})
        category = cls_info.get("category", "unknown")
        module = cls_info.get("module", "")
        source_file = cls_info.get("source_file", "")
        abi_info = self.abi_db.get(start, {})

        # ABI-derived info (kept for comments)
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")
        frame_type = abi_info.get("frame_type", "fpo_leaf")
        stack_frame_size = abi_info.get("stack_frame_size", 0)

        # Determine which registers are used
        used_regs = self._find_used_registers(instructions)
        used_xmm = self._find_used_xmm(instructions)
        has_prologue = self._func_has_prologue(instructions)
        has_fpu = any(insn.mnemonic.startswith("f") for insn in instructions)

        # Volatile registers (eax, ecx, edx, esp) are globals - don't declare
        # them as locals. The RECOMP_GENERATED_CODE #define maps register names
        # to the global variables via preprocessor macros.
        volatile_regs = {"eax", "ecx", "edx", "esp"}

        # Ensure ebp tracked if function uses 'leave' (implicit ebp)
        if any(insn.mnemonic == "leave" for insn in instructions):
            used_regs.add("ebp")

        # Guest control leaves the bottom of this function when its last
        # instruction neither returns, jumps, nor traps. A function the lifter
        # split into consecutive pieces continues into the next piece exactly
        # as an unconditional tail jump would, so bridge to it the same way.
        # Only an address that is itself a translated function start is a
        # usable target; anything else is an analysis boundary gap with no
        # callable symbol.
        last_insn = instructions[-1]
        continues_past_end = not (
            last_insn.is_terminator
            or last_insn.mnemonic in ("int3", "ud2", "hlt"))
        fallthrough_target = None
        if (continues_past_end and end in self.func_db
                and end not in self.owned_function_starts):
            fallthrough_target = end

        # Ensure ebp tracked if function has tail jumps (lifter emits
        # g_seh_ebp = ebp before external jmp, external jcc, and indirect jmp,
        # and translate_function emits it before a fallthrough tail call).
        has_tail_jump = any(
            (insn.mnemonic == "jmp" and (
                (insn.jump_target and not (start <= insn.jump_target < end))
                or not insn.jump_target  # indirect jmp
            ))
            or (insn.is_cond_jump and insn.jump_target
                and not (start <= insn.jump_target < end))
            for insn in instructions
        )
        if has_tail_jump or fallthrough_target is not None:
            used_regs.add("ebp")

        # External conditional branches can also enter a separately translated
        # fragment that shares the current native EBP frame. The lifter bridges
        # EBP on the taken path, so declare/initialize it just like a tail jump.
        has_external_cond_jump = any(
            insn.is_cond_jump and insn.jump_target and
            not (start <= insn.jump_target < end)
            for insn in instructions
        )
        if has_external_cond_jump:
            used_regs.add("ebp")

        # Ensure ebp is declared if the function talks to the SEH helpers: the
        # lifter emits a publish before and a read-back after those calls, both
        # of which name ebp even in a function that otherwise never touches it.
        #
        # These addresses are per-title and detected at startup (SCDA: via
        # SEH_FRAME_HELPERS, a broader detected set rather than just the two
        # named prolog/epilog addresses -- some titles route through more than
        # one shared frame helper). They used to be hardcoded to one game's
        # CRT here, so for every other title the forcing silently never fired
        # and the generated C failed to compile with "'ebp': undeclared
        # identifier".
        if any(insn.call_target in self.lifter.SEH_FRAME_HELPERS
               for insn in instructions):
            used_regs.add("ebp")

        # Build call targets list
        call_targets = set()
        for insn in instructions:
            if insn.call_target and is_code_address(insn.call_target):
                call_targets.add(insn.call_target)

        # All translated functions are void(void).
        # Arguments pass via the global simulated stack (push instructions).
        # Return values pass via g_eax (the global eax register).
        ret_type = "void"
        param_str = "void"

        # Generate C code
        lines = []

        # Header comment
        lines.append(f"/**")
        lines.append(f" * {name}")
        lines.append(f" * Original: 0x{start:08X} - 0x{end:08X} ({size} bytes, {len(instructions)} insns)")
        if category != "unknown":
            lines.append(f" * Category: {category}")
        if source_file:
            lines.append(f" * Source: {source_file}")
        lines.append(f" * CC: {cc}, {num_params} params, returns {return_hint}")
        if frame_type == "ebp_frame":
            lines.append(f" * Frame: EBP-based ({stack_frame_size} bytes locals)")
        else:
            lines.append(f" * Frame: {frame_type}")
        lines.append(f" */")

        # Function signature
        lines.append(f"{ret_type} {name}({param_str})")
        lines.append(f"{{")

        # Optional entry trace. Bring-up is mostly "which of these ten init
        # calls does it not come back from", and answering that by overriding
        # a function loses the body you were trying to observe.
        if start in self.trace_functions:
            lines.append(
                f'    RECOMP_TRACE_ENTER("{name}", 0x{start:08X});')
        # Entry tracing shows what went in; it cannot show what came back, and
        # "this function returns with esi wrong" is exactly the question that
        # kept coming up. The lifter emits the matching exit trace at each ret.
        self.lifter.trace_exit_name = name if start in self.trace_functions else None

        # ebp is the only callee-saved register declared as a local.
        # ebx, esi, edi are global via #define macros (g_ebx, g_esi, g_edi)
        # and must NOT be declared locally, otherwise the local shadows
        # the global and cross-function register passing breaks.
        # Volatile registers (eax, ecx, edx, esp) are also global via macros.
        reg_decls = []
        if "ebp" in used_regs:
            reg_decls.append("ebp")
        if reg_decls:
            lines.append(f"    uint32_t {', '.join(reg_decls)};")

        # A function with no `push ebp; mov ebp, esp` prologue that still reads
        # ebp is addressing its *caller's* frame. MSVC emits these for shared
        # tails and helpers; Halo's CRT float formatting (sub_001DEC07) opens
        # with `cmp byte ptr [edx+0xe], 5` and goes straight to [ebp-0xa4].
        #
        # ebp is a per-function local, so without this it starts as garbage and
        # every [ebp-N] store lands wherever that points. In Halo that was the
        # fake TIB at Xbox VA 0: `mov [ebp-0xa2], bx` destroyed fs:[4], and the
        # next TLS lookup faulted, thousands of calls away from the cause.
        #
        # Inherit it instead. Frame-establishing functions publish g_ebp when
        # they execute `mov ebp, esp` (see the lifter), so the value is the
        # nearest enclosing frame -- which is exactly what the hardware ebp
        # would still hold. Deliberately not the same as making ebp global:
        # that also changes save/restore, and a callee that fails to restore
        # then corrupts its caller (tried; esp underflowed inside XapiStartup).
        if "ebp" in used_regs and self._func_has_prologue(instructions):
            # The prologue's first PUSH saves the incoming register before
            # MOV establishes this function's frame. It must not push an
            # uninitialized C local into the guest's saved-frame chain.
            lines.append("    ebp = g_ebp;  /* prologue saves caller's frame */")
        elif "ebp" in used_regs:
            lines.append("    ebp = g_ebp;  /* frameless: caller's frame */")

        # Add _flags variable if function has conditional instructions
        has_conditionals = any(
            insn.is_cond_jump or insn.mnemonic.startswith("set")
            or insn.mnemonic.startswith("cmov")
            for insn in instructions)
        if has_conditionals:
            lines.append(f"    int _flags = 0; /* fallback flag var */")

        # Flag snapshot temporaries: a cmp/test records its operands here,
        # zero- and sign-extended to the compare's own width, so the branch
        # tests what the compare actually saw. Declared whenever a cmp/test
        # exists - the consuming jcc can be in a later basic block, or absent.
        # bsf/bsr publish ZF through the same pair (the source, against 0), so
        # a function whose only flag-setter is a bit scan still needs them --
        # sub_000EEA10 in Wreckless is exactly `bsf eax, ecx; ret`.
        # cmpxchg belongs here too: it snapshots the compare it performed,
        # because eax may be replaced before the branch reads the result.
        if any(insn.mnemonic in ("cmp", "test", "bsf", "bsr", "cmpxchg",
                                 "lock cmpxchg")
               for insn in instructions):
            lines.append("    uint32_t _fa = 0, _fb = 0, _fzr = 0;")
            lines.append("    int32_t _fas = 0, _fbs = 0;")
            lines.append("    (void)_fa; (void)_fb; (void)_fzr; (void)_fas; (void)_fbs;")
            # Flag snapshot: a cmp/test that is not fused with its jcc records
            # its operands here, zero- and sign-extended to the compare's own
            # width, so the branch tests what the compare saw.


        # Add _cf for carry-dependent instructions.
        #
        # adc/sbb read CF directly, and so does a jb/jae whose flags came from
        # arithmetic rather than a cmp -- the bit-stream decoders MSVC emits
        # are nothing but "add reg,reg" followed by jae. Which setter a branch
        # reads is the lifter's tracking rule (CF_TRACKED), mirrored here so
        # only the functions that consume CF declare it. Ported from upstream
        # sp00nznet/xboxrecomp commit 2bfb9e05a8; see docs/SCDA_WORKLOG.md,
        # 2026-09-11. NEG defines CF too (commonly followed by `sbb reg, reg`
        # to materialize a boolean), but that path is gated in the lifter by
        # preserve_carry/needs_cf rather than needing "neg" listed here.
        has_carry = self._function_needs_cf(instructions)
        if has_carry:
            lines.append(f"    int _cf = 0; /* carry flag */")
        # Only functions that consume CF pay for producing it: an adc/sbb
        # reading a never-written _cf silently drops every carry, which
        # corrupts multi-word arithmetic (add/adc pairs) and the shr/adc
        # idiom MSVC emits for odd trailing elements.
        self.lifter.needs_cf = has_carry
        self.lifter.publishes_ebp = self._func_owns_a_frame(instructions)

        # SSE and MMX are architectural state, declared globally by the
        # runtime exactly like the GPRs and the x87 stack. Declaring either
        # here would shadow the global with a fresh zeroed local, so a value
        # produced in one block and read in the next -- a return value in
        # xmm0, an mm register carried across a branch -- would be lost.
        #
        # MMX used to get `uint64_t mm0, mm1, ...` here, from before the
        # instructions were implemented and the registers were only ever
        # written. With mm0..mm7 now real globals, that declaration expands
        # through the `#define mm0 g_mm0` alias into a local named g_mm0 that
        # shadows the register it is meant to be.

        # The x87 stack is architectural state and survives guest calls. Some
        # detector boundaries also split one original CRT helper into several
        # generated C functions, so function-local storage loses live ST values.
        if has_fpu:
            lines.extend(FP_STACK_MACROS)

        # For fpo_leaf functions that use ebp: initialize from g_seh_ebp.
        # In x86, these functions inherit EBP from their caller (typically
        # via a tail jump that shares the caller's frame). In our C translation,
        # ebp is a local variable that would start uninitialized, causing
        # crashes when the function reads MEM32(ebp + offset). The g_seh_ebp
        # global bridges ebp across function boundaries.
        if frame_type == "fpo_leaf" and "ebp" in used_regs and not has_prologue:
            lines.append(f"    ebp = g_seh_ebp; /* fpo_leaf: inherit caller's frame */")

        lines.append(f"")
        # Where the _fsvN flag-operand saves get declared if the body uses
        # them (lifter._materialize_flag_ops); only known after lifting.
        flag_save_decl_idx = len(lines)

        # Generate code for each basic block
        # Create a set of addresses that need labels
        label_addrs = set()
        for bb in blocks:
            for succ in bb.successors:
                label_addrs.add(succ)
        # Also add any jump targets within the function
        for insn in instructions:
            if insn.jump_target and start <= insn.jump_target < end:
                label_addrs.add(insn.jump_target)
        # Add switch table targets (indirect jmp with intra-function table)
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                switch_targets = self.lifter._analyze_switch_table(insn.operands)
                for t in switch_targets:
                    label_addrs.add(t)

        # Which blocks can reach each block. Flag state has to follow control
        # flow, not address order: an optimising compiler routinely lets a jcc
        # consume a `cmp` from a block that is not its immediate predecessor in
        # memory. Threading the state linearly then hands that jcc the flags of
        # whatever instruction happens to sit above it -- silently, and with a
        # perfectly plausible-looking condition.
        preds = {bb.start: set() for bb in blocks}
        for i, bb in enumerate(blocks):
            last = bb.instructions[-1] if bb.instructions else None
            if last is None:
                continue
            if last.jump_target in preds:
                preds[last.jump_target].add(bb.start)
            # A conditional jump also falls through; ret and an unconditional
            # jmp do not.
            leaves = last.is_ret or last.mnemonic in ("jmp", "int3", "ud2", "hlt")
            if not leaves and i + 1 < len(blocks):
                preds[blocks[i + 1].start].add(bb.start)

        # Loops need the flag states solved to a fixpoint first (see
        # _solve_block_flag_states); straight-line code keeps the plain walk.
        # Re-lifting is side-effect free apart from the unimplemented-
        # mnemonic tally, which is restored so reports do not double count.
        order = {bb.start: i for i, bb in enumerate(blocks)}
        in_states = None
        if any(order.get(p, -1) >= order[bb.start]
               for bb in blocks for p in preds[bb.start]):
            saved_unimpl = {k: list(v) for k, v in self.lifter.unimplemented.items()}
            in_states = _solve_block_flag_states(
                blocks, preds, start,
                lambda bb, st: lift_basic_block(self.lifter, bb, flag_state=st)[1])
            self.lifter.unimplemented = saved_unimpl

        out_state = {}
        for bb in blocks:
            # Emit label if this block is a branch target
            if bb.start in label_addrs or bb.start == start:
                # The trailing ';' is load-bearing: C requires a statement after
                # a label, and a block whose instructions all emit comments only
                # (a lone `cmp`, which just sets flags for the next jcc) would
                # otherwise produce `loc_X:` immediately before `}` and fail to
                # compile. The null statement costs nothing and is always valid.
                lines.append(f"loc_{bb.start:08X}: ;")

            # Optional, compile-time guest-PC tracing.  The runtime macro is a
            # no-op unless a game project explicitly supplies a handler and a
            # bounded address range.  Emit this for every basic block (rather
            # than only labelled blocks) so return sites after direct calls are
            # observable without editing generated C.
            # (SCDA note: not upstream yet -- this project's entire live-debug
            # workflow, recomp_manual.c's scda_trace_pc, depends on it. Do not
            # drop this when merging future upstream changes to this method.)
            lines.append(f"    RECOMP_TRACE_PC(0x{bb.start:08X}u);")

            # Inherit the flag state only when every predecessor agrees on it.
            # Blocks are walked in address order, so a back edge's predecessor
            # may not be computed yet -- treat that as unknown rather than
            # guessing, which costs a fallback condition and never a wrong one.
            sources = preds[bb.start]
            if in_states is not None:
                incoming = in_states[bb.start]
            elif bb.start == start or not sources:
                incoming = None
            elif all(p in out_state for p in sources):
                states = [out_state[p] for p in sources]
                incoming = states[0]
                for other in states[1:]:
                    incoming = _merge_flag_states(incoming, other)
                    if incoming is None:
                        break
            else:
                incoming = None

            stmts, out_state[bb.start] = lift_basic_block(
                self.lifter, bb, flag_state=incoming)
            for stmt in stmts:
                lines.append(f"    {stmt}")

            lines.append(f"")

        # Continue into the next function when control runs off the bottom.
        # (A heuristic function candidate may split one native routine into
        # adjacent translated fragments; when the final decoded instruction
        # falls through into a known function entry, the Xbox CPU would
        # continue there rather than return to the caller. `used_regs` above
        # already forces "ebp" in whenever fallthrough_target is set, so the
        # bridge below is always valid here.)
        if fallthrough_target is not None:
            ft_name = self.lifter._call_target_name(fallthrough_target)
            lines.append(f"    g_seh_ebp = ebp; {ft_name}(); return;"
                         f" /* fallthrough 0x{fallthrough_target:08X} */")
            lines.append(f"")

        # Insert _icall_esp save points before RECOMP_ICALL_SAFE arg pushes.
        # The pattern is: optional PUSH32 args, then
        # PUSH32(esp, <retva>); RECOMP_ICALL_SAFE(...).
        # We insert "uint32_t _icall_esp = g_esp;" before the first arg push.
        lines = _fixup_icall_esp_save(lines)

        if any("_fsv" in line for line in lines[flag_save_decl_idx:]):
            names = ", ".join(f"{n} = 0" for n in FLAG_SAVE_VARS)
            casts = " ".join(f"(void){n};" for n in FLAG_SAVE_VARS)
            lines.insert(flag_save_decl_idx,
                         f"    uint32_t {names}; /* flag operands saved "
                         f"across flag-preserving writes */")
            lines.insert(flag_save_decl_idx + 1, f"    {casts}")

        # Validate: comment out goto targets that reference missing labels
        # (dead code after unconditional jumps may reference non-existent labels)
        import re
        defined_labels = set()
        goto_lines = []
        for idx, line in enumerate(lines):
            lbl_match = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
            if lbl_match:
                defined_labels.add(lbl_match.group(1))
            goto_match = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
            if goto_match:
                goto_lines.append((idx, goto_match.group(1)))
        for idx, target in goto_lines:
            if target not in defined_labels:
                lines[idx] = lines[idx].replace(
                    f"goto {target};",
                    f"(void)0; /* goto {target} - dead code, label not in function */")

        # Ensure labels at end of function have a statement after them.
        # In C, a label must be followed by a statement; a comment alone is not
        # enough.  Walk backwards from the end and if the last real content is a
        # label (with only blank lines / comments after it), insert "(void)0;".
        _last_label_idx = None
        _has_stmt_after = False
        for _ri in range(len(lines) - 1, -1, -1):
            _s = lines[_ri].strip()
            if not _s:
                continue
            if _s.startswith("/*") and _s.endswith("*/"):
                continue
            if re.match(r'^loc_[0-9A-Fa-f]+:', _s):
                _last_label_idx = _ri
                break
            _has_stmt_after = True
            break
        if _last_label_idx is not None and not _has_stmt_after:
            lines.insert(_last_label_idx + 1, "    (void)0;")

        # Undefine FPU macros
        if has_fpu:
            lines.extend(FP_STACK_UNDEFS)

        lines.append(f"}}")
        lines.append(f"")

        return "\n".join(lines)

    def _find_used_registers(self, instructions):
        """Find which 32-bit registers are referenced by any instruction."""
        regs = set()
        reg_map = {
            "eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
            "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
            "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
            "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
            "esi": "esi", "si": "esi",
            "edi": "edi", "di": "edi",
            "ebp": "ebp", "bp": "ebp",
            "esp": "esp", "sp": "esp",
        }
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg in reg_map:
                    regs.add(reg_map[op.reg])
                elif op.type == "mem":
                    if op.mem_base and op.mem_base in reg_map:
                        regs.add(reg_map[op.mem_base])
                    if op.mem_index and op.mem_index in reg_map:
                        regs.add(reg_map[op.mem_index])
        return regs

    def _find_used_xmm(self, instructions):
        """Find which XMM and MMX registers are used."""
        regs = set()
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg:
                    if op.reg.startswith("xmm") or op.reg.startswith("mm"):
                        regs.add(op.reg)
        return regs


class BatchTranslator:
    """Translates multiple functions and writes C source files."""

    def __init__(self, xbe_path, func_json_path, labels_json_path=None,
                 identified_json_path=None, abi_json_path=None,
                 output_dir=None, seh_prolog=None, seh_epilog=None,
                 trace_functions=None):
        self.xbe_path = xbe_path
        self.output_dir = output_dir or os.path.join(
            os.path.dirname(__file__), "output")

        # Load XBE
        with open(xbe_path, "rb") as f:
            self.xbe_data = f.read()

        self.title = xbe_title(self.xbe_data, xbe_path)

        # Load function database
        with open(func_json_path, "r") as f:
            func_list = json.load(f)

        self.func_db = {}
        for func in func_list:
            addr = int(func["start"], 16)
            func["_addr"] = addr
            if "end" in func:
                func["end"] = int(func["end"], 16)
            self.func_db[addr] = func

        # Load labels
        self.label_db = {}
        if labels_json_path and os.path.exists(labels_json_path):
            with open(labels_json_path, "r") as f:
                labels = json.load(f)
            for lbl in labels:
                addr = int(lbl["address"], 16)
                self.label_db[addr] = lbl["name"]

        # Load classifications
        self.classification_db = {}
        if identified_json_path and os.path.exists(identified_json_path):
            with open(identified_json_path, "r") as f:
                identified = json.load(f)
            for entry in identified:
                addr = int(entry["start"], 16)
                self.classification_db[addr] = entry

        # Load ABI data
        self.abi_db = {}
        if abi_json_path and os.path.exists(abi_json_path):
            with open(abi_json_path, "r") as f:
                abi_list = json.load(f)
            for entry in abi_list:
                addr = int(entry["address"], 16)
                self.abi_db[addr] = entry

        # Detect the SEH helpers once here rather than per-Lifter, so the
        # result can be reported and overridden from the command line.
        if seh_prolog is None or seh_epilog is None:
            found_prolog, found_epilog = detect_seh_helpers(
                self.func_db, self.xbe_data, verbose=True)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.seh_prolog = seh_prolog
        self.seh_epilog = seh_epilog

        setjmp_fn, longjmp_fn = detect_setjmp_helpers(
            self.func_db, self.xbe_data, verbose=True)

        # Create translator
        self.translator = FunctionTranslator(
            self.xbe_data, self.func_db, self.label_db,
            self.classification_db, self.abi_db,
            seh_prolog=seh_prolog, seh_epilog=seh_epilog,
            setjmp_fn=setjmp_fn, longjmp_fn=longjmp_fn,
            trace_functions=trace_functions)
        self.translator.discover_static_indirect_targets()
        self.translator.discover_cfg_ownership()

    def get_functions_by_category(self, categories=None, exclude_categories=None):
        """
        Get function addresses filtered by category.
        Returns list of (addr, func_info) tuples.
        """
        result = []
        for addr, func_info in sorted(self.func_db.items()):
            if addr in self.translator.owned_function_starts:
                continue
            cls_info = self.classification_db.get(addr, {})
            cat = cls_info.get("category", "unknown")

            if categories and cat not in categories:
                continue
            if exclude_categories and cat in exclude_categories:
                continue

            result.append((addr, func_info))
        return result

    def _make_declaration(self, addr, name):
        """Generate a function declaration string.
        All translated functions are void(void) - args pass via stack,
        return values via g_eax."""
        return f"void {name}(void)"

    def translate_single(self, addr):
        """Translate a single function by address. Returns C code string."""
        func_info = self.func_db.get(addr)
        if not func_info:
            return None
        return self.translator.translate_function(addr, func_info)

    def translate_batch(self, func_list, output_file=None, max_funcs=None,
                        verbose=False):
        """
        Translate a batch of functions.

        func_list: list of (addr, func_info) tuples
        output_file: path to write combined C output
        max_funcs: limit number of functions
        verbose: print progress

        Returns dict with statistics.
        """
        os.makedirs(self.output_dir, exist_ok=True)

        func_list = [item for item in func_list
                     if item[0] not in self.translator.owned_function_starts]

        if max_funcs:
            func_list = func_list[:max_funcs]

        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
            "total_insns": 0,
        }

        c_chunks = []
        c_chunks.append("/**")
        c_chunks.append(f" * {_config.banner_name(getattr(self, 'title', None))}"
                        f" - Mechanically Translated Game Code")
        c_chunks.append(f" * Generated by tools/recomp from original Xbox x86 code.")
        c_chunks.append(f" * Functions: {len(func_list)}")
        c_chunks.append(" */")
        c_chunks.append("")
        c_chunks.append('#define RECOMP_GENERATED_CODE')
        c_chunks.append('#include "recomp_types.h"')
        c_chunks.append('#include <math.h>')
        c_chunks.append("")
        c_chunks.append("/* Forward declarations */")

        # Forward declarations
        for addr, func_info in func_list:
            name = func_info.get("name", f"sub_{addr:08X}")
            decl = self._make_declaration(addr, name)
            c_chunks.append(f"{decl};")
        c_chunks.append("")
        c_chunks.append("/* ═══════════════════════════════════════════════════ */")
        c_chunks.append("")

        # Translate each function
        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 100 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name} at 0x{addr:08X}...")

            code = self.translator.translate_function(addr, func_info)
            if code:
                c_chunks.append(code)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

                # Count instructions
                num_insns = func_info.get("num_instructions", 0)
                stats["total_insns"] += num_insns
            else:
                c_chunks.append(f"/* FAILED to translate {name} at 0x{addr:08X} */")
                c_chunks.append(f"void {name}(void) {{ /* translation failed */ }}")
                c_chunks.append("")
                stats["failed"] += 1

        # Write output
        if output_file is None:
            output_file = os.path.join(self.output_dir, "recompiled.c")

        output_text = "\n".join(c_chunks)
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(output_text)

        stats["output_file"] = output_file
        stats["output_size"] = len(output_text)

        return stats

    def translate_by_category(self, categories, output_prefix=None,
                              max_per_file=500, verbose=False):
        """
        Translate functions grouped by category, one file per category.
        Returns dict with per-category stats.
        """
        os.makedirs(self.output_dir, exist_ok=True)
        all_stats = {}

        for cat in categories:
            funcs = self.get_functions_by_category(categories={cat})
            if not funcs:
                continue

            prefix = output_prefix or cat
            out_file = os.path.join(self.output_dir, f"{prefix}.c")

            if verbose:
                print(f"\nCategory: {cat} ({len(funcs)} functions)")

            stats = self.translate_batch(
                funcs, output_file=out_file,
                max_funcs=max_per_file, verbose=verbose)
            all_stats[cat] = stats

        return all_stats

    def translate_batch_split(self, func_list, output_dir, chunk_size=1000,
                              header_name="recomp_funcs.h",
                              prefix="recomp", verbose=False, manual=None):
        """
        Translate functions into multiple .c files + a shared header.

        Generates:
          output_dir/recomp_funcs.h       - forward declarations for all functions
          output_dir/recomp_0000.c        - chunk 0
          output_dir/recomp_0001.c        - chunk 1
          ...
          output_dir/recomp_dispatch.c    - address -> function pointer table

        manual: addresses the project implements by hand. Their bodies are not
        emitted, so the hand-written definition is the one that links, but they
        are still declared and still count as defined for stub purposes. This
        is how a game replaces a recompiled XDK routine (a D3D8 entry point,
        say) with one that drives the host runtime instead of the hardware.

        Returns dict with stats and list of generated files.
        """
        import sys

        os.makedirs(output_dir, exist_ok=True)

        func_list = [item for item in func_list
                     if item[0] not in self.translator.owned_function_starts]
        manual = set(manual or ())
        # Hand the set to the lifter so a *direct* call to a replaced
        # function routes through recomp_lookup_manual too. Without this
        # the override only took effect through a function pointer, and
        # every direct caller silently reached the generated body.
        self.translator.lifter.manual_functions = manual
        manual_decls = {}

        # Translate all functions first, collecting results
        translations = []
        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
        }

        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 500 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name}...",
                      file=sys.stderr)

            if addr in manual:
                # Hand-written elsewhere: declare it, emit nothing.
                manual_decls[addr] = name
                continue

            code = self.translator.translate_function(addr, func_info)
            if code:
                translations.append((addr, name, code))
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")
            else:
                # Stub for failed translations
                stub = f"/* FAILED: {name} at 0x{addr:08X} */\n"
                stub += f"void {name}(void) {{ /* translation failed */ }}\n"
                translations.append((addr, name, stub))
                stats["failed"] += 1

        # Any address called but never defined needs a stub, or the link fails.
        # These are almost all mid-function entry points the function detector
        # did not split out: a call lands a few bytes inside (or just past) a
        # function it already found. Emitting an empty stub keeps the build
        # linking; hitting one at runtime is a silent no-op, so they are
        # reported and written to their own file rather than hidden among the
        # translated chunks.
        defined = {name for _, name, _ in translations}
        defined |= set(manual_decls.values())   # hand-written, but defined
        # Wrapped functions: the body is sub_X_gen, sub_X is the hand-written
        # wrapper that callers reach -- defined too, so no stub for it.
        wrapped = {addr: f"sub_{addr:08X}" for addr, name, _ in translations
                   if name == f"sub_{addr:08X}_gen"}
        defined |= set(wrapped.values())
        unresolved = {
            addr: name
            for addr, name in self.translator.lifter.referenced_calls.items()
            if name not in defined
        }
        stats["unresolved_stubs"] = len(unresolved)
        stats["manual_functions"] = len(manual_decls)
        # Instructions the lifter has no translation for become a comment, and
        # a comment is a silent no-op. Surfacing the tally is the difference
        # between "bsf is unimplemented" being a line of build output and being
        # a week of heap debugging.
        stats["unimplemented"] = {
            m: list(addrs)
            for m, addrs in self.translator.lifter.unimplemented.items()
        }

        # Generate header with all forward declarations
        header_path = os.path.join(output_dir, header_name)
        header_lines = [
            "/**",
            f" * {_config.banner_name(getattr(self, 'title', None))}"
            f" - Recompiled Function Declarations",
            f" * {stats['translated']} functions, auto-generated by tools/recomp",
            " */",
            "",
            "#ifndef RECOMP_FUNCS_H",
            "#define RECOMP_FUNCS_H",
            "",
            '#include "recomp_types.h"',
            "",
        ]
        for addr, name, _ in translations:
            decl = self._make_declaration(addr, name)
            header_lines.append(f"{decl};")

        if wrapped:
            header_lines.append("")
            header_lines.append("/* Hand-written wrappers around sub_X_gen (recomp_manual.c) */")
            for addr in sorted(wrapped):
                header_lines.append(f"void {wrapped[addr]}(void);  /* 0x{addr:08X} */")

        if manual_decls:
            header_lines.append("")
            header_lines.append("/* Hand-written overrides (defined by the project) */")
            for addr in sorted(manual_decls):
                header_lines.append(
                    f"void {manual_decls[addr]}(void);  /* 0x{addr:08X} */")

        if unresolved:
            header_lines.append("")
            header_lines.append("/* Unresolved call targets (stubbed) */")
            for addr in sorted(unresolved):
                header_lines.append(f"void {unresolved[addr]}(void);")

        header_lines.extend(["", "#endif /* RECOMP_FUNCS_H */", ""])

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("\n".join(header_lines))

        # recomp_types.h goes with it.
        #
        # recomp_funcs.h includes it, and a quoted include searches the
        # including file's own directory first, so putting it here is all it
        # takes for the generated code to compile. It used to live only in
        # templates/runtime/, which every new project discovered the same way:
        # `error C1083: Cannot open include file: 'recomp_types.h'`, then a hunt
        # through the tree. It is the runtime's register model, not something a
        # project writes, so the pipeline should hand it over like everything
        # else it generates.
        #
        # Never overwritten. A project that has edited this copy keeps its
        # edits across a regen, which is the opposite of how the .c files
        # behave -- but this is a header a project may reasonably touch, and
        # silently reverting someone's change on every regen is worse than
        # letting a stale one persist. Delete it to get the current one back.
        types_dst = os.path.join(output_dir, "recomp_types.h")
        types_src = os.path.join(os.path.dirname(__file__), "..", "..",
                                 "templates", "runtime", "recomp_types.h")
        if os.path.exists(types_dst):
            # Still never overwritten -- but say so when it has fallen behind.
            # The generated .c files include it with quotes, so this copy wins
            # over the template for them whatever the project's include order
            # is; a stale one silently hid new runtime declarations twice
            # (recomp_rdtsc, then recomp_rep_movs_fwd became implicit int
            # functions -- docs Update 345).
            try:
                with open(types_src, "r", encoding="utf-8") as a, \
                        open(types_dst, "r", encoding="utf-8") as b:
                    if a.read() != b.read():
                        print(f"  WARNING: {types_dst} differs from "
                              f"templates/runtime/recomp_types.h and shadows it "
                              f"for every generated file; remove or rename it to "
                              f"get the current runtime header", file=sys.stderr)
            except OSError:
                pass
        if not os.path.exists(types_dst):
            try:
                with open(types_src, "r", encoding="utf-8") as src:
                    with open(types_dst, "w", encoding="utf-8") as dst:
                        dst.write(src.read())
                print(f"  wrote {types_dst} (runtime register model)",
                      file=sys.stderr)
            except OSError as e:
                print(f"  WARNING: could not write recomp_types.h ({e}); copy "
                      f"it from templates/runtime/ by hand or the build will "
                      f"not find it", file=sys.stderr)

        # Split translations into chunks and write .c files
        generated_files = [header_path]
        chunks = [translations[i:i+chunk_size]
                  for i in range(0, len(translations), chunk_size)]

        # Remove chunk files a previous, larger run left behind. Projects glob
        # gen/*.c into their build, so a stale chunk keeps compiling: it still
        # defines the functions it held last time, and the build fails with a
        # wall of "redefinition; different basic types" pointing at generated
        # code that looks perfectly correct. Nothing else cleans them, and the
        # count only has to shrink once -- which it does the first time a
        # detector fix changes how many functions are found.
        for stale in sorted(glob.glob(os.path.join(output_dir,
                                                   f"{prefix}_[0-9][0-9][0-9][0-9].c"))):
            index = int(os.path.basename(stale)[len(prefix) + 1:-2])
            if index >= len(chunks):
                os.remove(stale)
                if verbose:
                    print(f"  removed stale chunk {os.path.basename(stale)}")

        for ci, chunk in enumerate(chunks):
            c_path = os.path.join(output_dir, f"{prefix}_{ci:04d}.c")
            c_lines = [
                "/**",
                f" * {_config.banner_name(getattr(self, 'title', None))}"
                f" - Recompiled code chunk {ci}",
                f" * Functions: {len(chunk)} "
                f"(0x{chunk[0][0]:08X} - 0x{chunk[-1][0]:08X})",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                '#include <math.h>',
                "",
            ]
            for addr, name, code in chunk:
                c_lines.append(code)

            with open(c_path, "w", encoding="utf-8") as f:
                f.write("\n".join(c_lines))
            generated_files.append(c_path)

            if verbose:
                print(f"  Wrote {c_path} ({len(chunk)} functions)",
                      file=sys.stderr)

        # Emit the stub bodies for call targets with no definition.
        if unresolved:
            stub_path = os.path.join(output_dir, f"{prefix}_stubs_unresolved.c")
            stub_lines = [
                "/**",
                " * Unresolved call target stubs",
                f" * {len(unresolved)} addresses called by translated code but not",
                " * detected as functions - typically mid-function entry points.",
                " * Auto-generated by tools/recomp.",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                "",
            ]
            stub_lines.append(
                "/* Each stub consumes the return address its caller pushed,")
            stub_lines.append(
                " * exactly as a real 'ret' would. An empty body leaves esp 4 bytes")
            stub_lines.append(
                " * low, and the caller then reads every subsequent stack slot off by")
            stub_lines.append(
                " * one - which surfaces far from here, as corrupted callee-saved")
            stub_lines.append(
                " * registers or a garbage local. Args are not popped: the callee's")
            stub_lines.append(
                " * stdcall byte count is read from the target's own bytes where")
            stub_lines.append(
                " * they end in a `ret N` -- guessing cdecl there silently walks")
            stub_lines.append(
                " * esp off by N on every call. */")
            stub_lines.append("")
            for addr in sorted(unresolved):
                popped = self.translator._stub_ret_bytes(addr)
                note = (f"ret {popped}" if popped else "not detected")
                stub_lines.append(
                    f"void {unresolved[addr]}(void) {{ "
                    f"/* 0x{addr:08X}: {note} */ "
                    f"recomp_func_t fn = recomp_lookup_manual(0x{addr:08X}u); "
                    f"if (fn) fn(); else {{ recomp_unresolved_stub_hit(0x{addr:08X}u); "
                    f"g_esp += {4 + popped}; }} "
                    f"/* SCDA: keep the manual-override hook, but still consume "
                    f"the caller's real ret-N argument bytes (not just the "
                    f"return address) when there is no manual override -- an "
                    f"untouched stub used to leak exactly 4 bytes of guest "
                    f"stack per call regardless of the real callee shape. */"
                    f"}}"
                )
            stub_lines.append("")

            with open(stub_path, "w", encoding="utf-8") as f:
                f.write("\n".join(stub_lines))
            generated_files.append(stub_path)

            if verbose:
                print(f"  Wrote {stub_path} ({len(unresolved)} stubs)",
                      file=sys.stderr)

        # Generate dispatch table.
        #
        # Hand-written functions belong in it too. They are declare-only here,
        # so they never reached `translations` and got no entry -- which means
        # a *direct* call to one linked fine by symbol while an *indirect* call
        # to the same address found nothing in recomp_lookup and was dropped.
        # That is a silent hole, and it grows with every function a project
        # implements natively: on Half-Life 2 it covered memcpy, memmove,
        # _initterm and atexit. The header already declares them.
        # Sorted by address: recomp_lookup binary-searches this array, so an
        # appended entry would silently break every lookup past it.
        dispatch_entries = sorted(
            list(translations) + [(addr, name, None)
                                  for addr, name in manual_decls.items()],
            key=lambda e: e[0])
        dispatch_path = os.path.join(output_dir, f"{prefix}_dispatch.c")
        self._write_dispatch_table(dispatch_entries, dispatch_path, header_name)
        generated_files.append(dispatch_path)

        stats["files"] = generated_files
        stats["num_chunks"] = len(chunks)
        stats["chunk_size"] = chunk_size
        return stats

    def _write_dispatch_table(self, translations, output_path, header_name):
        """
        Generate a dispatch table mapping Xbox VA -> function pointer.

        Uses a sorted array + binary search for O(log n) lookup.
        """
        lines = [
            "/**",
            # getattr: the dispatch writer is exercised directly by tests
            # that build no full translator, and a banner is not worth an
            # AttributeError.
            f" * {_config.banner_name(getattr(self, 'title', None))}"
            f" - Recompiled Function Dispatch Table",
            f" * Maps {len(translations)} Xbox VAs to translated function pointers.",
            " * Auto-generated by tools/recomp",
            " */",
            "",
            "#define RECOMP_DISPATCH_H",
            f'#include "{header_name}"',
            '#include <stddef.h>',
            '#include <stdlib.h>',
            "",
            "/* Generic function pointer type */",
            "typedef void (*recomp_func_t)(void);",
            "",
            "typedef struct {",
            "    uint32_t xbox_va;",
            "    recomp_func_t func;",
            "} recomp_entry_t;",
            "",
            f"static const recomp_entry_t g_recomp_table[] = {{",
        ]

        for addr, name, _ in translations:
            # Indirect calls to a wrapped function reach the wrapper as well.
            if name == f"sub_{addr:08X}_gen":
                name = f"sub_{addr:08X}"
            lines.append(f"    {{ 0x{addr:08X}u, (recomp_func_t){name} }},")

        addrs = [addr for addr, _, _ in translations]
        flat_base = min(addrs) if addrs else 0
        flat_span = (max(addrs) - flat_base + 1) if addrs else 0

        lines.extend([
            "};",
            "",
            f"static const size_t g_recomp_table_size = "
            f"{len(translations)};",
            "",
            "/* ----------------------------------------------------------------",
            " * Flat, directly-indexed dispatch.",
            " *",
            " * Microsoft's recompiler resolves an indirect branch with a single",
            " * `jmp qword ptr [r9 + r8*8]` -- one indexed load off a table keyed",
            " * by guest address, no compare and no miss path. This is that, in C.",
            " *",
            " * The binary search below is still here and still correct. It runs",
            " * ~log2(n) iterations per indirect call, which for this title is",
            f" * about {max(1, len(translations).bit_length())} branches every time the game calls through a",
            " * vtable. The flat table turns that into a bounds check and a load.",
            " *",
            " * Costs 8 bytes per byte of guest code span. Allocated with calloc so",
            " * the untouched middle stays uncommitted rather than resident.",
            " *",
            " * recomp_dispatch_init() is optional by design: if it is never called,",
            " * or the allocation fails, recomp_lookup silently keeps using the",
            " * binary search. Nothing else in the program has to know. That is also",
            " * why this does not pre-fill manual overrides -- they cannot be",
            " * enumerated portably, so RECOMP_ICALL still consults",
            " * recomp_lookup_manual first and this only replaces the search it used",
            " * to fall through to. Behaviour is identical by construction.",
            " * ---------------------------------------------------------------- */",
            "",
            f"static const uint32_t g_flat_base = 0x{flat_base:08X}u;",
            f"static const uint32_t g_flat_span = 0x{flat_span:08X}u;",
            "static recomp_func_t *g_flat_table = NULL;",
            "",
            "int recomp_dispatch_init(void)",
            "{",
            "    size_t i;",
            "    if (g_flat_table) return 1;          /* already built */",
            "    if (!g_flat_span) return 0;",
            "    g_flat_table = (recomp_func_t *)calloc(g_flat_span,",
            "                                           sizeof(recomp_func_t));",
            "    if (!g_flat_table) return 0;         /* keep the binary search */",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_flat_table[g_recomp_table[i].xbox_va - g_flat_base] =",
            "            g_recomp_table[i].func;",
            "    }",
            "    return 1;",
            "}",
            "",
            "size_t recomp_dispatch_flat_bytes(void)",
            "{",
            "    return g_flat_table ? (size_t)g_flat_span * sizeof(recomp_func_t) : 0;",
            "}",
            "",
            "/* Flat index when built, binary search otherwise. */",
            "recomp_func_t recomp_lookup(uint32_t xbox_va)",
            "{",
            "    size_t lo, hi;",
            "    if (g_flat_table) {",
            "        uint32_t off = xbox_va - g_flat_base;",
            "        /* Unsigned: a VA below the base wraps to a huge offset and is",
            "         * rejected by the same compare, so no separate lower bound. */",
            "        return (off < g_flat_span) ? g_flat_table[off] : NULL;",
            "    }",
            "    lo = 0; hi = g_recomp_table_size;",
            "    while (lo < hi) {",
            "        size_t mid = lo + (hi - lo) / 2;",
            "        if (g_recomp_table[mid].xbox_va < xbox_va)",
            "            lo = mid + 1;",
            "        else if (g_recomp_table[mid].xbox_va > xbox_va)",
            "            hi = mid;",
            "        else",
            "            return g_recomp_table[mid].func;",
            "    }",
            "    return NULL;",
            "}",
            "",
            "/* Get the number of registered functions */",
            "size_t recomp_get_count(void)",
            "{",
            "    return g_recomp_table_size;",
            "}",
            "",
            "/* Call all registered functions (for bulk testing) */",
            "size_t recomp_call_all(void)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_recomp_table[i].func();",
            "    }",
            "    return g_recomp_table_size;",
            "}",
            "",
        ])

        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
