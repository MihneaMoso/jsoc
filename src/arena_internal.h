#pragma once

/*
 * Arena tail access, internal to the library.
 *
 * `jsoc_encode_object` writes its text into the unallocated tail of the
 * arena's current chunk when the document fits, which skips the size pass
 * entirely.  Writing there is safe because the bytes are owned by the arena
 * and nobody else can allocate them: the caller must commit exactly the bytes
 * it keeps, before any other allocation happens.
 */

#include "arena.h"
#include "export.h"

#include <stddef.h>

JSOC_INTERNAL char *jsoc_arena_tail(jsoc_arena *a, size_t *length);
JSOC_INTERNAL char *jsoc_arena_commit(jsoc_arena *a, size_t used);
