# SCDA: optional boot-video data patch (experimental)

`skip_boot_videos_dynamicxbox.ips` changes only two configuration values
inside `DynamicXbox.umd`:

| Key | Original | Replacement |
|---|---|---|
| `LogosVideo` | `logos.bik` | `nope!.bik` |
| `SplashVideo` | `teaser.bik` | `nope!!.bik` |

The named replacement files intentionally do not exist. In an isolated
Recomp test, the game's own missing-file path skipped both videos and
continued to `back_menu.bik` without a crash. The first **Press START**
screen still requires START; this patch does not skip it or any mission video.

This 37-byte classic IPS patch is for a **copy** of `DynamicXbox.umd`,
not `default.xbe`. It was generated and replay-verified against a source
archive with SHA-256
`D7AE7DF5FCF09E65BCE2CFDA24299B55644FD9DBACBE2176ED99AC53055C1868`.
The patched archive has SHA-256
`3694F6C021BCFE7172366E9660B49E98A55ECFC88478F0FA94D7B86CEBA0AC8B`.
Do not apply this IPS blindly to a different UMD revision or to a file
with other changes. For another revision, the validated script
`scratchpad/tools/prepare_scda_intro_umd.py` can create a new isolated
copy by locating the exact configuration strings.

Recomp may play from its cached `Cache/Default/DynamicXbox.umd`, so
patching only the disc-side copy need not affect an already populated
cache. The current normal launcher and normal cache have **not** been
changed; no automatic enable/disable switch exists yet.

Evidence: `docs/SCDA_WORKLOG.md`, Update 607, and the isolated logs
`scratchpad/bink_cache_20261003/newgame_intro_config_both_missing_runtime.log`.
