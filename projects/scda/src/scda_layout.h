#ifndef SCDA_LAYOUT_H
#define SCDA_LAYOUT_H

#include <stddef.h>

/* Release folder layout (docs Update 659), next to scda_recomp.exe:
 *
 *   gamefiles\   the game: an Xbox ISO (XISO or full disc image), unpacked
 *                once on the first start, or an unpacked game folder with
 *                default.xbe (directly inside or one folder below)
 *   user\        everything the game and the runtime write: UDATA, TDATA,
 *                CACHE, the emulated hard disk (hdd\), logs, shader cache
 *
 * Both folders are created when missing. Without game files a message box
 * explains what to do and the process exits.
 *
 * Returns 1 and fills game_dir/user_dir (ANSI code page) in the release
 * layout; returns 0 when default.xbe lies next to the EXE (the development
 * layout, unchanged) and touches nothing then. */
int scda_layout_resolve(const char *exe_dir, char *game_dir, size_t game_size,
                        char *user_dir, size_t user_size);

#endif
