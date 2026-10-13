# Save games from xemu or a real Xbox (and back)

Splinter Cell: Double Agent signs every used save file with 20 bytes at its
end, computed from two keys:

- the **game key** (`XboxSignatureKey`, derived by the Xbox kernel from the
  game - the same on every retail console), and
- the **console key** (`XboxHDKey`, from the console's EEPROM - different on
  every Xbox and every xemu installation).

The files are not encrypted, only signed. If the signature does not match
the keys of the machine, the game treats the save as damaged.

SCDA Recomp ships **no keys** (they are Microsoft/console data). Without a
key file it signs with all-zero keys: its own saves work, saves from xemu or
a console do not - until you either give it your keys or re-sign the saves.

Requirements: Python 3 (`py -3`), the scripts in this folder.

## 1. Read your keys (once)

From **xemu**:
1. Start xemu with its debugger port: `xemu.exe -s`
2. Start Splinter Cell: Double Agent and wait for the main menu (the game key
   only exists while the game runs).
3. `py -3 tools\saves\xemu_read_kernel_keys.py xbox_keys.bin`

This writes `xbox_keys.bin` (32 bytes: game key, then console key).

For a **real Xbox**, do the same in an xemu that uses that console's own
EEPROM (`eeprom.bin`) - the console key then matches your Xbox.

Keep `xbox_keys.bin` to yourself. Never upload or share it.

## 2. Get the save files

- **xemu** (close xemu first):
  `py -3 tools\saves\xemu_hdd_fatx_ls.py path\to\xbox_hdd.qcow2 5553005e --extract out`
  copies the game's save folder to `out\P1\UDATA\5553005e\`.
- **Real Xbox**: copy `E:\UDATA\5553005e\` with FTP from a modded console.

`5553005e` is the game's title ID. Inside are the profile folders (12 hex
characters, e.g. `07C8B760B46D`), each with `profile.sav`, `SaveMeta.xbx` and
the slots `single0.sav` ... `multi5.sav` plus their `...image.sav`.

## 3. Use them in SCDA Recomp

Copy the whole profile folder into `user\UDATA\5553005e\` next to
`scda_recomp.exe`. Always move a complete profile folder - single slot files
from another profile hang the game while loading. Then either:

- **A (recommended):** put `xbox_keys.bin` next to `scda_recomp.exe`.
  SCDA Recomp then signs exactly like your xemu/console, so saves stay
  usable in both directions without further steps.
- **B:** re-sign the profile for SCDA Recomp's zero keys, no key file needed
  afterwards:
  `py -3 tools\saves\xbox_resign_saves.py user\UDATA\5553005e\<profile> --from xbox_keys.bin --to zero`

Add `--dry-run` to see first what would change. Empty slots carry no
signature and are skipped - that is normal.

## 4. Back to xemu or the Xbox

- With variant A the saves are already signed correctly.
- Saves made with zero keys: re-sign them first:
  `py -3 tools\saves\xbox_resign_saves.py <profile folder> --from zero --to xbox_keys.bin`
- **xemu:** write them into a *copy* of the HDD image (close xemu first):
  `py -3 tools\saves\xemu_hdd_fatx_put.py xbox_hdd_copy.qcow2 1 UDATA\5553005e\<profile>\profile.sav <local profile.sav> ...`
  The tool only overwrites files that already exist in the image (create the
  profile and save once in xemu first) and refuses anything it does not
  understand. Copy `profile.sav`, `SaveMeta.xbx` and the slot files together.
- **Real Xbox:** copy the profile folder back via FTP.
