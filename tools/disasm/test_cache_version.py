"""
Self-check for AnalysisCache's version gate.

Run: py -3 tools/disasm/test_cache_version.py

Regression guard for 2026-09-09's finding: tools/disasm/output/scda/
functions.json sat stale for eleven days after a real function-boundary
fix (ffe089d/3381db5/c4a7ebf/aeb406b) landed in functions.py, because
nothing ever re-ran the disassembler for that directory in between --
_opts_key()'s source hash is the primary guard against a stale result
being *accepted*, but it only helps if the tool actually runs again.
CACHE_VERSION is the explicit backstop: a cache file written under an
older version must never be treated as valid, and a cache written under
the current version with identical inputs must still be treated as
valid (otherwise every run would be a full re-analysis).
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm import config  # noqa: E402
from tools.disasm.cache import AnalysisCache  # noqa: E402


def _make_inputs(tmpdir):
    """A tiny fake XBE and analysis JSON -- the cache only hashes bytes,
    it never parses either file, so any content is fine."""
    xbe_path = os.path.join(tmpdir, "fake.xbe")
    json_path = os.path.join(tmpdir, "functions.json")
    with open(xbe_path, "wb") as f:
        f.write(b"fake xbe contents")
    with open(json_path, "w") as f:
        f.write("[]")
    # is_valid() also requires these to exist in output_dir.
    for fname in ("summary.json", "xrefs.json", "strings.json", "labels.json"):
        open(os.path.join(tmpdir, fname), "w").close()
    return xbe_path, json_path


def test_old_cache_version_is_invalid():
    with tempfile.TemporaryDirectory() as tmpdir:
        xbe_path, json_path = _make_inputs(tmpdir)
        cache = AnalysisCache(tmpdir)
        cache.save(xbe_path, json_path, text_only=True, elapsed_seconds=1.0)

        # Simulate a cache written under an older CACHE_VERSION by patching
        # the saved file directly, the way an on-disk leftover from before
        # a version bump would look.
        import json as _json
        with open(cache.cache_file) as f:
            data = _json.load(f)
        data["version"] = config.CACHE_VERSION - 1
        with open(cache.cache_file, "w") as f:
            _json.dump(data, f)

        stale_cache = AnalysisCache(tmpdir)
        assert not stale_cache.is_valid(xbe_path, json_path, text_only=True), (
            "a cache file from an older CACHE_VERSION must be rejected")


def test_current_cache_version_is_valid_for_identical_inputs():
    with tempfile.TemporaryDirectory() as tmpdir:
        xbe_path, json_path = _make_inputs(tmpdir)
        cache = AnalysisCache(tmpdir)
        cache.save(xbe_path, json_path, text_only=True, elapsed_seconds=1.0)

        # A fresh AnalysisCache instance (not the one that just wrote it) --
        # exercises the on-disk load path, not just the in-memory one.
        reloaded = AnalysisCache(tmpdir)
        assert reloaded.is_valid(xbe_path, json_path, text_only=True), (
            "a cache saved under the current CACHE_VERSION, with identical "
            "inputs, must be accepted")


def test_changed_input_still_invalidates_regardless_of_version():
    # Sanity check that bumping CACHE_VERSION did not accidentally weaken
    # the existing xbe/json/opts hash checks.
    with tempfile.TemporaryDirectory() as tmpdir:
        xbe_path, json_path = _make_inputs(tmpdir)
        cache = AnalysisCache(tmpdir)
        cache.save(xbe_path, json_path, text_only=True, elapsed_seconds=1.0)

        with open(xbe_path, "wb") as f:
            f.write(b"different xbe contents")

        changed = AnalysisCache(tmpdir)
        assert not changed.is_valid(xbe_path, json_path, text_only=True), (
            "a changed input file must invalidate the cache regardless of "
            "CACHE_VERSION"
        )


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print("cache version: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
