/* Only candidate VM translation units include this. The allocator itself does
 * not. Preload declarations before function-like allocation macros. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include "next_arena.h"
#define malloc vm_arena_malloc
#define calloc vm_arena_calloc
#define realloc vm_arena_realloc
#define free vm_arena_free
#define strdup vm_arena_strdup
#define strndup vm_arena_strndup
