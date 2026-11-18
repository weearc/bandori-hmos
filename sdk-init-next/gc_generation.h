/* GC-GEN-R2: shadow infrastructure only. Not a replacement for dvm_ref. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t gen_ref;
#define DVM_GEN_SLOT_CAPACITY 4194304u
/* Raw bit operations preserve ALL inputs, including invalid slot/ticket 0.
 * Validation is separate, inline and never throws a guest exception. */
static inline gen_ref gen_encode(uint32_t slot,uint32_t ticket){return ((uint64_t)ticket<<32)|slot;}
static inline uint32_t gen_slot(gen_ref ref){return (uint32_t)ref;}
static inline uint32_t gen_ticket(gen_ref ref){return (uint32_t)(ref>>32);}
static inline bool gen_ref_valid(gen_ref ref){return (gen_slot(ref)!=0)&(gen_ticket(ref)!=0);}
enum gen_result {GEN_SUCCESS,GEN_INVALID_ARGUMENT,GEN_SLOT_CAPACITY,
 GEN_TICKET_EXHAUSTED,GEN_ALLOCATION_FAILED,GEN_STALE_REFERENCE};
struct gen_shadow;
struct gen_counters {uint64_t gen_issued,gen_reuse_bumps,gen_check_ok,gen_check_stale,gen_overflow;};
/* Publication callers and offline selftests. All VMs share the
 * private process bank; there is deliberately no reset/reseed API. Writers
 * and destruction require exclusive ownership (future VM GIL). Reads are
 * atomic but must not race table destruction. heap_alloc publishes only;
 * ticket_of/check remain disconnected from production queries. */
enum gen_result gen_shadow_create(uint32_t capacity,struct gen_shadow **out);
void gen_shadow_destroy(struct gen_shadow *);
enum gen_result gen_shadow_publish(struct gen_shadow *,uint32_t slot,uint32_t *ticket_out);
uint32_t gen_ticket_of(const struct gen_shadow *,uint32_t slot);
bool gen_check(const struct gen_shadow *,uint32_t slot,uint32_t expected_ticket);
/* Worker-only counters: snapshot never issues tickets/checks/allocates. */
void gen_snapshot(struct gen_counters *);
