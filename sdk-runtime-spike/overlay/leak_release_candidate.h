#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct dvm;
void lr_start(void);
bool lr_enabled(void);
unsigned lr_register(struct dvm *,uint32_t,uint32_t,int64_t);
void lr_returned(unsigned);
void lr_poll(struct dvm *);
void lr_forget(struct dvm *);
void lr_worker(void);
void lr_frame(uint64_t);
void lr_arena_released(void *,size_t);
void lr_denied(void);
