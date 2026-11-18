#pragma once
#include <stdint.h>
enum { GCO_HOST, GCO_POP, GCO_GUEST_BEGIN, GCO_GUEST_END,
       GCO_INJECT_END, GCO_ENQUEUE, GCO_REJECT, GCO_CONTRACT, GCO_STAGES };
#define GCO_PRODUCERS 64u
#define GCO_RING_CAP 256u
#define GCO_REASONS 17u
struct gco_row {
 uint64_t seq,begin,end;
 int32_t action,a,b,c;
 uint32_t stage,producer;
};
struct gco_snapshot {
 uint64_t counts[GCO_STAGES][8],reasons[GCO_REASONS],dropped;
 unsigned enabled,producers;
};
/* Producers: no allocation, clocks, syscalls, locks or IO. */
void gco_emit(unsigned,uint64_t,int,int,int,int,uint64_t,uint64_t);
void gco_contract_failure(unsigned);
/* Existing worker only: controls, drain and atomic snapshots. */
void gco_set_enabled(unsigned);
int gco_take(unsigned,struct gco_row *);
void gco_read_snapshot(struct gco_snapshot *);
