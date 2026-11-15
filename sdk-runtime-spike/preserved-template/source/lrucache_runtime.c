#include "lrucache_runtime.h"
#include "java_basics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

typedef struct lru_entry {dvm_ref key,value;int size;struct lru_entry *prev,*next;} lru_entry;
typedef struct {int max,size,hit,miss,put,evict;lru_entry *head,*tail;} lru;
static void lru_free(void *p){
 lru *c=p;if(!c)return;lru_entry *e=c->head;while(e){lru_entry *n=e->next;free(e);e=n;}free(c);
}
static bool is_lru(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);for(struct dvm_class*k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"android/util/LruCache"))return true;return false;
}
static lru_entry *find(lru *c,dvm_ref key){for(lru_entry*e=c->head;e;e=e->next)if(e->key==key)return e;return NULL;}
static lru_entry *find_equal(struct dvm *v,lru *c,dvm_ref key){
 for(lru_entry*e=c->head;e;e=e->next){if(java_basics_equal(v,e->key,key))return e;if(dvm_exception(v))return NULL;}return NULL;
}
static void unlink(lru *c,lru_entry *e){
 if(e->prev)e->prev->next=e->next;else c->head=e->next;
 if(e->next)e->next->prev=e->prev;else c->tail=e->prev;
 e->prev=e->next=NULL;
}
static void push_head(lru *c,lru_entry *e){e->prev=NULL;e->next=c->head;if(c->head)c->head->prev=e;else c->tail=e;c->head=e;}
static void touch(lru *c,lru_entry *e){if(c->head!=e){unlink(c,e);push_head(c,e);}}
static int size_of(struct dvm *v,dvm_ref self,dvm_ref key,dvm_ref value){
 struct dvm_object *o=dvm__obj(v,self);
 struct dvm_method *m=o?dvm_find_method(v,o->cls,"sizeOf","(Ljava/lang/Object;Ljava/lang/Object;)I"):NULL;
 if(m&&(m->has_code||m->builtin)){union dvm_value a[2]={{.l=key},{.l=value}},r={0};if(dvm_call(v,m,self,a,2,&r)&&!dvm_exception(v)){if(r.i<0){dvm__throw(v,"java/lang/IllegalStateException","negative sizeOf");return -1;}return r.i;}}
 return 1;
}
static dvm_ref create_value(struct dvm *v,dvm_ref self,dvm_ref key){
 struct dvm_object *o=dvm__obj(v,self);
 struct dvm_method *m=o?dvm_find_method(v,o->cls,"create","(Ljava/lang/Object;)Ljava/lang/Object;"):NULL;
 if(!m||(!m->has_code&&!m->builtin))return 0;
 union dvm_value a[1]={{.l=key}},r={0};
 if(!dvm_call(v,m,self,a,1,&r)||dvm_exception(v))return 0;
 return r.l;
}
/* Returns false when the guest callback threw.  The cache mutation already
 * performed by the caller stays in place, as it does in Android LruCache. */
static bool entry_removed(struct dvm *v,dvm_ref self,int evicted,dvm_ref key,dvm_ref old,dvm_ref new_value){
 struct dvm_object *o=dvm__obj(v,self);
 struct dvm_method *m=o?dvm_find_method(v,o->cls,"entryRemoved","(ZLjava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V"):NULL;
 if(m&&(m->has_code||m->builtin)){union dvm_value a[4]={{.i=evicted},{.l=key},{.l=old},{.l=new_value}},r={0};dvm_call(v,m,self,a,4,&r);if(dvm_exception(v))return false;}
 return true;
}
static bool trim(struct dvm *v,dvm_ref self,lru *c){
 while(c->size>c->max&&c->tail){
  lru_entry *e=c->tail;unlink(c,e);c->size-=e->size;c->evict++;
  bool ok=entry_removed(v,self,1,e->key,e->value,0);
  free(e);
  if(!ok)return false;
 }
 return true;
}
void lrucache_runtime_vm_start(struct dvm *v){
 (void)v;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool lrucache_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)c;(void)n;
 if(!is_lru(v,self))return false;
 struct dvm_object *o=dvm__obj(v,self);lru *cache=o&&o->data_destroy==lru_free?o->data:NULL;
 if(IS("<init>","(I)V")){
  if(cache){dvm__throw(v,"java/lang/IllegalStateException","LruCache already initialized");return true;}
  if(a[0].i<=0){dvm__throw(v,"java/lang/IllegalArgumentException","maxSize <= 0");return true;}
  cache=calloc(1,sizeof *cache);if(!cache){dvm__throw(v,"java/lang/OutOfMemoryError","LruCache");return true;}
  cache->max=a[0].i;o->data=cache;o->data_destroy=lru_free;return true;
 }
 if(!cache){dvm__throw(v,"java/lang/IllegalStateException","LruCache constructor not called");return true;}
 if(IS("maxSize","()I")){out->i=cache->max;return true;}
 if(IS("size","()I")){out->i=cache->size;return true;}
 if(IS("hitCount","()I")){out->i=cache->hit;return true;}
 if(IS("missCount","()I")){out->i=cache->miss;return true;}
 if(IS("putCount","()I")){out->i=cache->put;return true;}
 if(IS("evictionCount","()I")){out->i=cache->evict;return true;}
 if(IS("get","(Ljava/lang/Object;)Ljava/lang/Object;")){
  lru_entry *e=find_equal(v,cache,a[0].l);if(dvm_exception(v))return true;
  if(!e){
   cache->miss++;
   dvm_ref created=create_value(v,self,a[0].l);
   if(dvm_exception(v))return true;
   if(!created){out->l=0;return true;}
   /* The create override runs without the cache locked and may have put the
    * same key.  Re-check before inserting a second entry for that key. */
   lru_entry *now=find_equal(v,cache,a[0].l);if(dvm_exception(v))return true;
   if(now){
    dvm_ref existing=now->value;
    touch(cache,now);
    if(!entry_removed(v,self,0,a[0].l,created,existing))return true;
    out->l=existing;return true;
   }
   int size=size_of(v,self,a[0].l,created);
   if(dvm_exception(v))return true;
   /* sizeOf can also be overridden by the guest; re-check after it. */
   now=find_equal(v,cache,a[0].l);if(dvm_exception(v))return true;
   if(now){
    dvm_ref existing=now->value;
    touch(cache,now);
    if(!entry_removed(v,self,0,a[0].l,created,existing))return true;
    out->l=existing;return true;
   }
   e=calloc(1,sizeof *e);if(!e){dvm__throw(v,"java/lang/OutOfMemoryError","LruCache create entry");return true;}
   e->key=a[0].l;e->value=created;e->size=size;
   push_head(cache,e);cache->size+=size;cache->put++;
   if(!trim(v,self,cache))return true;
   out->l=created;return true;
  }
  cache->hit++;touch(cache,e);out->l=e->value;return true;
 }
 if(IS("put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")){
  if(!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","LruCache key/value");return true;}
  /* sizeOf is a guest override.  Resolve the current old entry only after it
   * returns: a reentrant put/remove from that callback may already have
   * replaced or freed the node that was in the cache before this call. */
  int size=size_of(v,self,a[0].l,a[1].l);
  if(dvm_exception(v))return true;
  lru_entry *old=find_equal(v,cache,a[0].l);if(dvm_exception(v))return true;
  dvm_ref previous=old?old->value:0;
  lru_entry *e=calloc(1,sizeof *e);if(!e){dvm__throw(v,"java/lang/OutOfMemoryError","LruCache entry");return true;}
  e->key=a[0].l;e->value=a[1].l;e->size=size;
  /* Match Android LruCache.put(): the new mapping is installed before
   * entryRemoved runs.  If the callback puts this same key, that nested put
   * replaces/frees this node and the outer call must not insert again. */
  if(old){unlink(cache,old);cache->size-=old->size;free(old);}
  push_head(cache,e);cache->size+=size;cache->put++;
  if(previous){
   if(!entry_removed(v,self,0,a[0].l,previous,a[1].l))return true;
  }
  if(!trim(v,self,cache))return true;
  out->l=previous;return true;
 }
 if(IS("remove","(Ljava/lang/Object;)Ljava/lang/Object;")){
  lru_entry *e=find_equal(v,cache,a[0].l);if(dvm_exception(v))return true;
  if(!e){out->l=0;return true;}
  unlink(cache,e);cache->size-=e->size;
  dvm_ref previous=e->value;
  bool ok=entry_removed(v,self,0,a[0].l,previous,0);
  free(e);
  if(!ok)return true;
  out->l=previous;return true;
 }
 if(IS("evictAll","()V")){
  /* Detach the whole list before invoking guest callbacks. A legal
   * entryRemoved may re-enter remove/evictAll/put; it must never see a node
   * that this loop is still about to free. */
  lru_entry *e=cache->head;
  cache->head=cache->tail=NULL;cache->size=0;
  bool ok=true;
  while(e){
   lru_entry *next=e->next;e->prev=e->next=NULL;
   if(ok)ok=entry_removed(v,self,1,e->key,e->value,0);
   free(e);e=next;
  }
  return true;
 }
 return false;
}
/* Selftest subclass: each entry counts as three units. */
static bool lru_negative;
static dvm_ref lru_reentry_cache,lru_reentry_key,lru_reentry_value;
static int lru_reentry_mode,lru_removed_calls;
static int lru_probe,lru_probe_seen,lru_probe_size,lru_probe_evicted;
static dvm_ref lru_probe_key,lru_probe_old,lru_probe_new;
static int lru_create_mode;
static dvm_ref lru_create_value;
static bool lru_size_three(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)v;(void)self;(void)a;(void)n;out->i=lru_negative?-1:3;return true;}
static bool lru_create_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;
 if(lru_create_mode){
  int mode=lru_create_mode;lru_create_mode=0;
  union dvm_value args[2]={{.l=a[0].l},{.l=lru_create_value}},r={0};
  if(mode==1){
   lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",self,args,2,&r);
   if(dvm_exception(v))return true;
  }else if(mode==2){
   dvm__throw(v,"java/lang/RuntimeException","create boom");return true;
  }
 }
 out->l=dvm_new_string(v,"created");return true;
}
static bool lru_entry_removed_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;(void)out;lru_removed_calls++;
 if(lru_probe){
  union dvm_value sz={0};
  lru_probe=0;lru_probe_seen=1;lru_probe_evicted=a[0].i;lru_probe_key=a[1].l;lru_probe_old=a[2].l;lru_probe_new=a[3].l;
  if(lrucache_runtime_call(v,"android/util/LruCache","size","()I",self,NULL,0,&sz))lru_probe_size=sz.i;
 }
 if(lru_reentry_mode){
  int mode=lru_reentry_mode;lru_reentry_mode=0;
  union dvm_value key={.l=lru_reentry_key},r={0};
  if(mode==1)lrucache_runtime_call(v,"android/util/LruCache","remove","(Ljava/lang/Object;)Ljava/lang/Object;",lru_reentry_cache,&key,1,&r);
  else if(mode==2)lrucache_runtime_call(v,"android/util/LruCache","evictAll","()V",lru_reentry_cache,NULL,0,&r);
  else if(mode==3){union dvm_value args[2]={{.l=lru_reentry_key},{.l=lru_reentry_value}};lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",lru_reentry_cache,args,2,&r);}
  else if(mode==4){dvm__throw(v,"java/lang/RuntimeException","entryRemoved boom");return true;}
 }
 return true;
}
static dvm_ref lru_test_new(struct dvm *v,struct dvm_class *sub){
 dvm_ref cache=dvm_new_object(v,sub);union dvm_value max={.i=10},out={0};
 if(!cache||!lrucache_runtime_call(v,"android/util/LruCache","<init>","(I)V",cache,&max,1,&out))return 0;
 return cache;
}
static bool lru_test_put(struct dvm *v,dvm_ref cache,dvm_ref key,dvm_ref value,dvm_ref *previous){
 union dvm_value a[2]={{.l=key},{.l=value}},out={0};
 if(!lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",cache,a,2,&out)||dvm_exception(v))return false;
 if(previous)*previous=out.l;
 return true;
}
static bool lru_test_get(struct dvm *v,dvm_ref cache,dvm_ref key,dvm_ref *value){
 union dvm_value a[1]={{.l=key}},out={0};
 if(!lrucache_runtime_call(v,"android/util/LruCache","get","(Ljava/lang/Object;)Ljava/lang/Object;",cache,a,1,&out)||dvm_exception(v))return false;
 if(value)*value=out.l;
 return true;
}
static bool lru_test_size(struct dvm *v,dvm_ref cache,int *size){
 union dvm_value out={0};
 if(!lrucache_runtime_call(v,"android/util/LruCache","size","()I",cache,NULL,0,&out))return false;
 if(size)*size=out.i;
 return true;
}
bool lrucache_runtime_selftest(struct dvm *v){
 struct dvm_class *base=dvm_find_class_or_external(v,"android/util/LruCache");
 struct dvm_class *sub=dvm__register_builtin(v,"Lsdktrace/tests/LruSub;");
 sub->super=base;sub->methods=calloc(3,sizeof *sub->methods);sub->nmethods=3;
 sub->methods[0].cls=sub;sub->methods[0].name="sizeOf";sub->methods[0].sig=strdup("(Ljava/lang/Object;Ljava/lang/Object;)I");sub->methods[0].ret_kind='I';sub->methods[0].builtin=lru_size_three;
 sub->methods[1].cls=sub;sub->methods[1].name="create";sub->methods[1].sig=strdup("(Ljava/lang/Object;)Ljava/lang/Object;");sub->methods[1].ret_kind='L';sub->methods[1].builtin=lru_create_builtin;
 sub->methods[2].cls=sub;sub->methods[2].name="entryRemoved";sub->methods[2].sig=strdup("(ZLjava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V");sub->methods[2].ret_kind='V';sub->methods[2].builtin=lru_entry_removed_builtin;
 dvm_ref cache=lru_test_new(v,sub);
 if(!cache)return false;
 union dvm_value out={0};
 dvm_ref key=dvm_new_string(v,"k"),value=dvm_new_string(v,"v");
 union dvm_value put[2]={{.l=key},{.l=value}};
 if(!lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",cache,put,2,&out)||out.l)return false;
 if(!lrucache_runtime_call(v,"android/util/LruCache","size","()I",cache,NULL,0,&out)||out.i!=3)return false;
 union dvm_value get[1]={{.l=key}};
 if(!lrucache_runtime_call(v,"android/util/LruCache","get","(Ljava/lang/Object;)Ljava/lang/Object;",cache,get,1,&out)||out.l!=value)return false;
 /* A miss with a create override must populate the cache. */
 dvm_ref fresh_key=dvm_new_string(v,"fresh");
 union dvm_value fresh_get[1]={{.l=fresh_key}};
 if(!lrucache_runtime_call(v,"android/util/LruCache","get","(Ljava/lang/Object;)Ljava/lang/Object;",cache,fresh_get,1,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"created"))return false;
 if(!lrucache_runtime_call(v,"android/util/LruCache","size","()I",cache,NULL,0,&out)||out.i!=6)return false;
 dvm_ref key2=dvm_new_string(v,"k2"),value2=dvm_new_string(v,"v2");
 union dvm_value put2[2]={{.l=key2},{.l=value2}};
 if(!lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",cache,put2,2,&out)||out.l)return false;
 /* entryRemoved re-enters remove() while evictAll is running. */
 lru_reentry_cache=cache;lru_reentry_key=key2;lru_reentry_mode=1;lru_removed_calls=0;
 if(!lrucache_runtime_call(v,"android/util/LruCache","evictAll","()V",cache,NULL,0,&out))return false;
 lru_reentry_mode=0;
 if(dvm_exception(v)||lru_removed_calls<2)return false;
 if(!lrucache_runtime_call(v,"android/util/LruCache","size","()I",cache,NULL,0,&out)||out.i)return false;
 /* Negative sizeOf must be rejected instead of producing a corrupt size. */
 lru_negative=1;
 dvm_ref key3=dvm_new_string(v,"neg"),value3=dvm_new_string(v,"negv");
 union dvm_value put3[2]={{.l=key3},{.l=value3}};
 bool negative_rejected=lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",cache,put3,2,&out)&&dvm_exception(v);
 lru_negative=0;dvm_clear_exception(v);
 if(!negative_rejected)return false;
 if(!lrucache_runtime_call(v,"android/util/LruCache","maxSize","()I",cache,NULL,0,&out)||out.i!=10)return false;

 /* entryRemoved -> put(same key): the nested put must replace the outer
  * value, and the outer put must not append a duplicate entry. */
 dvm_ref cache_r=lru_test_new(v,sub);if(!cache_r)return false;
 dvm_ref rk=dvm_new_string(v,"rk"),rv=dvm_new_string(v,"rv"),rv2=dvm_new_string(v,"rv2"),rv3=dvm_new_string(v,"rv3");
 if(!lru_test_put(v,cache_r,rk,rv,NULL))return false;
 lru_reentry_cache=cache_r;lru_reentry_key=rk;lru_reentry_value=rv3;lru_reentry_mode=3;lru_removed_calls=0;
 dvm_ref previous=0;
 if(!lru_test_put(v,cache_r,rk,rv2,&previous)||previous!=rv)return false;
 if(dvm_exception(v))return false;
 int size=0;
 if(!lru_test_size(v,cache_r,&size)||size!=3)return false;
 dvm_ref got=0;
 if(!lru_test_get(v,cache_r,rk,&got)||got!=rv3)return false;
 if(lru_removed_calls!=2)return false;

 /* create -> put(same key): get()'s miss path must notice the entry created
  * by its own callback and return the callback's value, not insert "created"
  * as a duplicate. */
 dvm_ref cache_c=lru_test_new(v,sub);if(!cache_c)return false;
 dvm_ref ck=dvm_new_string(v,"ck"),cv=dvm_new_string(v,"cv");
 lru_create_mode=1;lru_create_value=cv;lru_removed_calls=0;
 dvm_ref created_out=0;
 if(!lru_test_get(v,cache_c,ck,&created_out)||created_out!=cv)return false;
 if(!lru_test_size(v,cache_c,&size)||size!=3)return false;
 if(lru_removed_calls!=1)return false;
 if(!lru_test_get(v,cache_c,ck,&got)||got!=cv)return false;

 /* entryRemoved callback sees the already-installed mapping and the old
  * value argument. */
 dvm_ref cache_q=lru_test_new(v,sub);if(!cache_q)return false;
 dvm_ref qk=dvm_new_string(v,"qk"),qk2=dvm_new_string(v,"qk2"),qv=dvm_new_string(v,"qv"),qv2=dvm_new_string(v,"qv2"),qv3=dvm_new_string(v,"qv3");
 if(!lru_test_put(v,cache_q,qk,qv,NULL)||!lru_test_put(v,cache_q,qk2,qv2,NULL))return false;
 lru_probe=1;lru_probe_seen=0;lru_probe_size=-1;lru_probe_evicted=-1;lru_probe_key=lru_probe_old=lru_probe_new=0;
 previous=0;
 if(!lru_test_put(v,cache_q,qk,qv3,&previous)||previous!=qv)return false;
 if(dvm_exception(v))return false;
 if(!lru_probe_seen||lru_probe_size!=6||lru_probe_evicted!=0||lru_probe_key!=qk||lru_probe_old!=qv||lru_probe_new!=qv3)return false;
 if(!lru_test_size(v,cache_q,&size)||size!=6)return false;
 if(!lru_test_get(v,cache_q,qk,&got)||got!=qv3)return false;

 /* A throwing entryRemoved propagates, but the replacement mapping that was
  * installed before the callback stays in the cache. */
 dvm_ref cache_e=lru_test_new(v,sub);if(!cache_e)return false;
 dvm_ref ek=dvm_new_string(v,"ek"),ev=dvm_new_string(v,"ev"),ev2=dvm_new_string(v,"ev2");
 if(!lru_test_put(v,cache_e,ek,ev,NULL))return false;
 union dvm_value put_args[2]={{.l=ek},{.l=ev2}},put_out={0};
 lru_reentry_mode=4;lru_removed_calls=0;
 bool removed_threw=lrucache_runtime_call(v,"android/util/LruCache","put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",cache_e,put_args,2,&put_out);
 if(!removed_threw||!dvm_exception(v)){lru_reentry_mode=0;return false;}
 dvm_clear_exception(v);lru_reentry_mode=0;
 if(lru_removed_calls!=1)return false;
 if(!lru_test_size(v,cache_e,&size)||size!=3)return false;
 if(!lru_test_get(v,cache_e,ek,&got)||got!=ev2)return false;

 /* A throwing create() propagates and leaves no entry behind. */
 dvm_ref cache_x=lru_test_new(v,sub);if(!cache_x)return false;
 dvm_ref xk=dvm_new_string(v,"xk");
 union dvm_value xget[1]={{.l=xk}},xout={0};
 lru_create_mode=2;
 bool create_threw=lrucache_runtime_call(v,"android/util/LruCache","get","(Ljava/lang/Object;)Ljava/lang/Object;",cache_x,xget,1,&xout)&&dvm_exception(v);
 dvm_clear_exception(v);
 if(!create_threw)return false;
 if(!lru_test_size(v,cache_x,&size)||size!=0)return false;
 if(!lru_test_get(v,cache_x,xk,&created_out)||!created_out||strcmp(dvm_string_utf8(v,created_out),"created"))return false;

 sdk_trace_fprintf(stderr,"LRUCACHE_RUNTIME_SELFTEST PASS constructor=1 sizeOf_override=1 get_put=1 create_on_miss=1 evictAll_reentrant=1 negative_size_rejected=1 put_reentry_same_key=1 create_reentry_same_key=1 removed_callback_probe=1 removed_callback_exception=1 create_callback_exception=1\n");
 return true;
}
