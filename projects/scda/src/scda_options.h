/* scda_options.txt next to the executable (docs Update 614). */
#pragma once

/* Reads (or creates) scda_options.txt and turns its settings into the
 * RECOMP_* environment variables that are not set yet. Call first in main. */
void scda_options_load(void);
