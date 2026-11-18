/* Cooperative boundary between complete events; never preempts a guest call.
 * Limits apply even with diagnostics off. Counters/ring use existing opt-in. */
#define TOUCH_DRAIN_MAX_EVENTS 32u
#define TOUCH_DRAIN_MAX_NS 2000000ull
struct touch_batch {uint64_t start,max_event_ns;unsigned events,limit;};
static atomic_uint_fast64_t touch_batch_calls,touch_batch_nonempty,touch_batch_count_hits,touch_batch_time_hits;
static atomic_uint_fast64_t touch_batch_max_events,touch_batch_max_ns,touch_batch_max_event_ns,touch_batch_yields;
static void touch_batch_max(atomic_uint_fast64_t *p,uint64_t value){
 uint64_t old=atomic_load_explicit(p,memory_order_relaxed);
 while(old<value&&!atomic_compare_exchange_weak_explicit(p,&old,value,memory_order_relaxed,memory_order_relaxed)){}
}
static struct touch_batch touch_batch_begin(uint64_t now){return (struct touch_batch){.start=now};}
static int touch_batch_limit(struct touch_batch *b,uint64_t now){
 b->limit=(b->events>=TOUCH_DRAIN_MAX_EVENTS?1u:0u)|
  (b->events&&now>=b->start&&now-b->start>=TOUCH_DRAIN_MAX_NS?2u:0u);
 return b->limit!=0;
}
static void touch_batch_event(struct touch_batch *b,uint64_t begin,uint64_t end){
 ++b->events;if(end>=begin&&end-begin>b->max_event_ns)b->max_event_ns=end-begin;
}
static void touch_batch_finish(const struct touch_batch *b,uint64_t end){
 if(!atomic_load_explicit(&input_obs_enabled,memory_order_relaxed))return;
 uint64_t serial=atomic_fetch_add_explicit(&touch_batch_calls,1,memory_order_relaxed)+1;
 if(b->events)atomic_fetch_add_explicit(&touch_batch_nonempty,1,memory_order_relaxed);
 if(b->limit&1)atomic_fetch_add_explicit(&touch_batch_count_hits,1,memory_order_relaxed);
 if(b->limit&2)atomic_fetch_add_explicit(&touch_batch_time_hits,1,memory_order_relaxed);
 touch_batch_max(&touch_batch_max_events,b->events);
 touch_batch_max(&touch_batch_max_ns,end>=b->start?end-b->start:0);
 touch_batch_max(&touch_batch_max_event_ns,b->max_event_ns);
 input_obs_record(INPUT_DRAIN_BATCH,serial,(int)b->limit,(int)b->events,b->start,end);
}
static void touch_batch_write(FILE *f){
 fprintf(f,"drain_batch_event_limit=%u drain_batch_time_limit_ns=%llu cooperative_between_events=1 batch_calls=%llu nonempty_batches=%llu event_limit_hits=%llu time_limit_hits=%llu max_batch_events=%llu max_batch_ns=%llu max_single_event_ns=%llu batch_gil_yields=%llu\n",
  TOUCH_DRAIN_MAX_EVENTS,(unsigned long long)TOUCH_DRAIN_MAX_NS,
  (unsigned long long)atomic_load(&touch_batch_calls),(unsigned long long)atomic_load(&touch_batch_nonempty),
  (unsigned long long)atomic_load(&touch_batch_count_hits),(unsigned long long)atomic_load(&touch_batch_time_hits),
  (unsigned long long)atomic_load(&touch_batch_max_events),(unsigned long long)atomic_load(&touch_batch_max_ns),
  (unsigned long long)atomic_load(&touch_batch_max_event_ns),(unsigned long long)atomic_load(&touch_batch_yields));
}
