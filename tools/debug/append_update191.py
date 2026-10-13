worklog_path = "docs/SCDA_WORKLOG.md"

worklog_text = """
## Update 191 (2026-09-20): Frame-pointer recompiler fix confirmed present and rebuilt, does NOT resolve the crash; corrected a same-run-vs-cross-run memory-diff methodology error

**Synced with upstream agent's work:** HEAD moved past Update 190 to `79e6c19`, a cherry-picked upstream fix ("Initialize the incoming frame pointer before a classic prologue saves it") in `tools/recomp/translator.py`. Confirmed this is a real, previously-missing initialization: functions with a classic `push ebp; mov ebp, esp` prologue were pushing an indeterminate C local as the saved frame pointer instead of the caller's actual `ebp`, corrupting the guest frame-pointer chain on return. The generated sources under `tools/recomp/output/scda_v060/gen/` already reflect this fix (mtime newer than `translator.py`), and `scda_recomp.exe`/`.pdb` were already rebuilt from those sources.

**Tested directly: this fix does not resolve the current boot blocker.** A fresh run with the rebuilt binary reproduces the exact same failure sequence as every prior test this investigation: `disc short read: zeroed 41103 ...` immediately followed by `NtReadFile(handle=0000000000000000, len=2048, bytes_read=0) failed err=6`, landing in the same fatal callback with the same zeroed ring-buffer subobject. This is a real, valuable systemic correctness fix for the recompiler, but it is not the root cause of this specific bug (or not sufficient alone).

**Methodology correction (important):** attempted a wide-region (128 KiB) memory diff around the confirmed subobject address (`0x1135928`) comparing a "GOOD" snapshot against a "ZERO" snapshot. The first attempt compared snapshots from *two different process instances* and found them byte-for-byte identical except for trivial ASLR-varying values -- this looked like evidence of a large deterministic "arena wipe," but was actually an artifact: the "GOOD" snapshot was captured via a separate, later `cdb` invocation than the initial vtable check that confirmed it was good, and the multi-second gap between the two commands was enough for the real transition to already have happened. Both "GOOD" and "ZERO" cross-process dumps were therefore already post-transition, and their surface identity says nothing about determinism of surrounding heap content.

**Corrected test:** a single `cdb -pv` invocation that checks `vtable==0x4F6940` and immediately dumps the same 128 KiB region in one command, run once early (confirmed GOOD) and once again against the *same* process instance after it reached the fatal callback (confirmed ZERO). This proper same-process comparison shows: the object's own 0x4068-byte block (`0x1135928` to `0x1139990`) cleanly transitions from real constructed values to all-zero, and the zeroing extends at least 16 bytes before the object's nominal start (`0x1135918` held real string/metadata content in the GOOD dump, already zero in the BAD dump). The much wider surrounding region also differs substantially between the two snapshots, but the BAD-side content there looks like high-entropy data (plausibly real compressed audio/disc bytes from an unrelated, later stream) rather than zero -- consistent with ordinary heap churn over the several-second gap between snapshots, not a targeted bulk clear. The Update 190 "blanket memory reset" hypothesis is therefore retracted as unsupported; it was inferred from the flawed cross-process comparison.

**Current process left running for the next session:** `scda_recomp.exe` PID 29872 is still alive, sitting in the `sub_0003FCA5` `jmp $` loop, and is the source of the two valid same-process dumps (`tools/debug/wide_good_tight.txt`, `tools/debug/wide_zero_samepid.txt`, and their extracted `dd`-line-only forms `good_dd_lines.txt`/`zero_samepid_dd_lines.txt`). Do not kill it -- further non-invasive reads against this exact instance remain valid for continuing the boundary search.

**Also validated a lighter-weight transition-narrowing technique:** repeated fast non-invasive `cdb -pv` polls (no breakpoints, ~1s per poll due to cdb startup cost) against a single numeric value can bound the GOOD->ZERO transition to roughly a 1-second window (`tools/debug/poll_subobj_out3.log`, transition between poll #21 and #22) without any of the severe per-hit overhead that scripted conditional breakpoints have shown repeatedly in this sandbox (confirmed again this session: an unfiltered `sub_0004438C` breakpoint and even a filtered `sub_00040821` conditional both ran far below native speed, in one case producing zero forward progress in debuggee CPU time over several minutes of wall clock).

**Recommendation logged in `PROJECT_STATUS.md`'s CURRENT HANDOFF:** escalate to a stronger model/reasoning tier for the next step, since this now requires careful heap-allocator forensics (distinguishing genuine clearing from ordinary free-list/allocation churn) rather than mechanical build/test work, and a real methodology mistake was already made and self-caught this round.
"""

with open(worklog_path, "rb") as f:
    raw = f.read()
raw += worklog_text.replace("\n", "\r\n").encode("utf-8")
with open(worklog_path, "wb") as f:
    f.write(raw)

print("done")
