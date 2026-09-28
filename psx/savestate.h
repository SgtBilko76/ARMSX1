#ifndef SAVESTATE_H
#define SAVESTATE_H

#include "psx.h"

/* Save or restore the whole emulated machine (see savestate.c). Both return 0 on success; a
   failed load leaves the machine untouched unless it fails midway on an allocation. */
int psx_state_save(psx_t* psx, const char* path);
int psx_state_load(psx_t* psx, const char* path);

#endif
