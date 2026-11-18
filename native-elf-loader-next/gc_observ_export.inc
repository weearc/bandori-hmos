/* Included by the EXISTING diagnostic worker implementation only. Exporter
 * reads observation buffers; it never invokes either producer symbol. */
#include "gc_observ_core.h"
#if defined(fopen) || defined(write) || defined(malloc) || defined(fsync) || defined(vdprintf)
#error GC_OBSERV_EXPORT_MUST_CALL_LIBC_WITHOUT_INTERPOSITION
#endif
static void gco_worker_export(void){
 char path[640];snprintf(path,sizeof path,"%s/gc-observ-control.txt",root);
 FILE *f=fopen(path,"r");unsigned on=0;if(f){if(fchmod(fileno(f),0644)){fclose(f);return;}fscanf(f,"%u",&on);fclose(f);}gco_set_enabled(on);
 struct gco_snapshot s;gco_read_snapshot(&s);
 static int initialized;
 snprintf(path,sizeof path,"%s/gc-observ-input.csv",root);f=fopen(path,initialized?"a":"w");
 if(f){if(fchmod(fileno(f),0644)){fclose(f);return;}
  if(!initialized){fputs("pid,seq,stage,action,begin_ns,end_ns,producer,a,b,c\n",f);initialized=1;}
  for(unsigned i=0;i<GCO_PRODUCERS;i++){
   struct gco_row q;for(unsigned n=0;n<GCO_RING_CAP&&gco_take(i,&q);n++)
    fprintf(f,"%d,%llu,%u,%d,%llu,%llu,%u,%d,%d,%d\n",getpid(),(unsigned long long)q.seq,q.stage,q.action,(unsigned long long)q.begin,(unsigned long long)q.end,q.producer,q.a,q.b,q.c);
  }fclose(f);
 }
 snprintf(path,sizeof path,"%s/gc-observ-status.txt",root);f=fopen(path,"w");if(!f)return;
 if(fchmod(fileno(f),0644)){fclose(f);return;}
 fprintf(f,"pid=%d mono_ns=%llu enabled=%u producers=%u producer_cap=%u ring_cap=%u ring_bytes=%zu dropped=%llu producer_io=0 producer_alloc=0 producer_locks=0\n",getpid(),(unsigned long long)diag_now(),s.enabled,s.producers,GCO_PRODUCERS,GCO_RING_CAP,(size_t)GCO_PRODUCERS*(2*sizeof(_Atomic unsigned)+GCO_RING_CAP*sizeof(struct gco_row)),(unsigned long long)s.dropped);
 for(unsigned i=0;i<GCO_STAGES;i++)for(unsigned a=0;a<8;a++)fprintf(f,"count_%u_%u=%llu\n",i,a,(unsigned long long)s.counts[i][a]);
 for(unsigned i=0;i<GCO_REASONS;i++)fprintf(f,"reason_%u=%llu\n",i,(unsigned long long)s.reasons[i]);
 fclose(f);
}
