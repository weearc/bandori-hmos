/* Stateful Java base-class contracts. No SDK/business code is substituted.
 * Java SE contract references are recorded in README.md. All calls occur under
 * the interpreter GIL. AtomicBoolean additionally uses C11 seq_cst atomics. */
#include "java_basics.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>

enum {PAY_LIST=1,PAY_ITER,PAY_BOOL,PAY_TLS,PAY_TIMEUNIT,PAY_ATOMIC_INT};
struct boxed_integer {int32_t value;};
struct boxed_long {int64_t value;};
static struct boxed_integer *boxed_integer_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,"java/lang/Integer")?(struct boxed_integer *)o->data:NULL;
}
static struct boxed_long *boxed_long_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,"java/lang/Long")?(struct boxed_long *)o->data:NULL;
}
struct snapshot {size_t owners,n;dvm_ref values[];};
struct tls_entry {pthread_t thread;dvm_ref value;struct tls_entry *next;};
struct payload {int kind;struct snapshot *s;size_t pos;_Atomic bool flag;_Atomic int32_t number;struct tls_entry *tls;uint64_t unit_nanos;};
static struct snapshot *snapshot_new(size_t n) {
 if(n>1000000)return NULL;
 struct snapshot *s=calloc(1,sizeof(*s)+n*sizeof(dvm_ref));
 if(s){s->owners=1;s->n=n;}return s;
}
static void snapshot_release(struct snapshot *s){if(s&&!--s->owners)free(s);}
static void destroy(void *data){
 struct payload *p=data;snapshot_release(p->s);
 while(p->tls){struct tls_entry *e=p->tls;p->tls=e->next;free(e);}free(p);
}
static struct payload *payload(struct dvm *v,dvm_ref ref,int kind,bool create){
 struct dvm_object *o=dvm__obj(v,ref);if(!o){dvm__throw(v,"java/lang/NullPointerException","base class receiver");return NULL;}
 struct payload *p=o->data;
 if(!p&&create){p=calloc(1,sizeof *p);if(p){p->kind=kind;atomic_init(&p->flag,false);atomic_init(&p->number,0);o->data=p;o->data_destroy=destroy;}}
 if(!p){dvm__throw(v,"java/lang/IllegalStateException","base class constructor not called");return NULL;}
 if(p->kind!=kind){dvm__throw(v,"java/lang/IllegalStateException","base class payload kind mismatch");return NULL;}
 return p;
}
bool java_basics_equal(struct dvm *v,dvm_ref a,dvm_ref b){
 if(a==b)return true;if(!a||!b)return false;
 struct dvm_object *x=dvm__obj(v,a),*y=dvm__obj(v,b);
 if(x&&y&&x->kind==DVM_OBJ_STRING&&y->kind==DVM_OBJ_STRING)return x->utf8_len==y->utf8_len&&!memcmp(x->utf8,y->utf8,x->utf8_len);
 struct boxed_integer *ix=boxed_integer_of(v,a),*iy=boxed_integer_of(v,b);
 if(ix||iy)return ix&&iy&&ix->value==iy->value;
 struct dvm_method *m=x?dvm_find_method(v,x->cls,"equals","(Ljava/lang/Object;)Z"):NULL;
 if(m){union dvm_value arg={.l=b},r={0};return dvm_call(v,m,a,&arg,1,&r)&&r.i;}
 if(x&&x->cls&&x->cls->external&&strcmp(x->cls->name,"java/lang/Object")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","equals contract not implemented for external class %s",x->cls->name);
 }
 /* java.lang.Object.equals defaults to reference identity, already tested. */
 return false;
}
bool java_basics_static(struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *out){
 if(!strcmp(c,"java/util/concurrent/TimeUnit")&&!strcmp(t,"Ljava/util/concurrent/TimeUnit;")){
  static const char *names[]={"NANOSECONDS","MICROSECONDS","MILLISECONDS","SECONDS","MINUTES","HOURS","DAYS"};
  static const uint64_t scales[]={1,1000,1000000,1000000000,60000000000ULL,3600000000000ULL,86400000000000ULL};
  int index=-1;for(int i=0;i<7;i++)if(!strcmp(f,names[i]))index=i;if(index<0)return false;
  struct dvm_class *unit=dvm_find_class_or_external(v,"java/util/concurrent/TimeUnit");
  if(!unit){dvm__throw(v,"java/lang/NoClassDefFoundError","java/util/concurrent/TimeUnit");return true;}
  out->l=dvm_new_object(v,unit);if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","TimeUnit.SECONDS");return true;}
  struct payload *p=payload(v,out->l,PAY_TIMEUNIT,true);if(!p)return true;p->unit_nanos=scales[index];
  return true;
 }
 if(strcmp(f,"TYPE")||strcmp(t,"Ljava/lang/Class;"))return false;
 static const struct {const char *name,*desc;} types[]={
  {"java/lang/Void","V"},{"java/lang/Boolean","Z"},{"java/lang/Byte","B"},{"java/lang/Character","C"},{"java/lang/Short","S"},{"java/lang/Integer","I"},{"java/lang/Long","J"},{"java/lang/Float","F"},{"java/lang/Double","D"}};
 for(size_t i=0;i<sizeof(types)/sizeof(types[0]);++i)if(!strcmp(c,types[i].name)){
  out->l=dvm_class_object(v,dvm__define_primitive(v,types[i].desc));return true;
 }
 return false;
}
bool java_basics_timeunit_nanos(struct dvm *v,dvm_ref unit,uint64_t *nanos){
 struct dvm_object *o=dvm__obj(v,unit);struct payload *p=o?o->data:NULL;
 if(!o||o->data_destroy!=destroy||!p||p->kind!=PAY_TIMEUNIT)return false;
 *nanos=p->unit_nanos;return *nanos!=0;
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
bool java_basics_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 (void)nargs;memset(out,0,sizeof *out);
 if(!strcmp(c,"java/util/concurrent/TimeUnit")&&nargs==1&&!strcmp(sig,"(J)J")){
  static const char *methods[]={"toNanos","toMicros","toMillis","toSeconds","toMinutes","toHours","toDays"};
  static const uint64_t divisors[]={1,1000,1000000,1000000000,60000000000ULL,3600000000000ULL,86400000000000ULL};
  int index=-1;for(int i=0;i<7;i++)if(!strcmp(m,methods[i]))index=i;
  if(index<0)return false;uint64_t scale;if(!java_basics_timeunit_nanos(v,self,&scale))return false;
  __int128 result=(__int128)a[0].j*scale/divisors[index];out->j=result>INT64_MAX?INT64_MAX:result<INT64_MIN?INT64_MIN:(int64_t)result;return true;
 }
 if(!strcmp(c,"java/util/concurrent/atomic/AtomicInteger")){
  bool ctor=IS("<init>","()V")||IS("<init>","(I)V");
  bool get=IS("get","()I")||IS("intValue","()I");
  bool set=IS("set","(I)V")||IS("lazySet","(I)V");
  bool cas=IS("compareAndSet","(II)Z")||IS("weakCompareAndSet","(II)Z");
  bool exchange=IS("getAndSet","(I)I");
  bool inc=IS("getAndIncrement","()I")||IS("incrementAndGet","()I");
  bool dec=IS("getAndDecrement","()I")||IS("decrementAndGet","()I");
  bool add=IS("getAndAdd","(I)I")||IS("addAndGet","(I)I");
  bool text=IS("toString","()Ljava/lang/String;");
  if(!ctor&&!get&&!set&&!cas&&!exchange&&!inc&&!dec&&!add&&!text)return false;
  struct payload *p=payload(v,self,PAY_ATOMIC_INT,ctor);if(!p)return true;
  if(ctor){atomic_store(&p->number,IS("<init>","(I)V")?a[0].i:0);return true;}
  if(get)out->i=atomic_load(&p->number);
  else if(set)atomic_store(&p->number,a[0].i);
  else if(cas){int32_t expected=a[0].i;out->i=atomic_compare_exchange_strong(&p->number,&expected,a[1].i);}
  else if(exchange)out->i=atomic_exchange(&p->number,a[0].i);
  else if(inc||dec||add){
   int32_t delta=inc?1:dec?-1:a[0].i;int32_t old=atomic_fetch_add(&p->number,delta);
   bool after=IS("incrementAndGet","()I")||IS("decrementAndGet","()I")||IS("addAndGet","(I)I");
   out->i=after?(int32_t)((uint32_t)old+(uint32_t)delta):old;
  }else{char buf[24];snprintf(buf,sizeof buf,"%"PRId32,atomic_load(&p->number));out->l=dvm_new_string(v,buf);}
  return true;
 }
 if(!strcmp(c,"java/lang/String")&&!strcmp(m,"valueOf")&&nargs==1){
  char text[32];
  if(!strcmp(sig,"(I)Ljava/lang/String;"))snprintf(text,sizeof text,"%"PRId32,a[0].i);
  else if(!strcmp(sig,"(J)Ljava/lang/String;"))snprintf(text,sizeof text,"%"PRId64,a[0].j);
  else if(!strcmp(sig,"(Z)Ljava/lang/String;"))snprintf(text,sizeof text,"%s",a[0].i?"true":"false");
  else return false;
  out->l=dvm_new_string(v,text);return true;
 }
 if(!strcmp(c,"java/lang/Integer")){
  bool value_of=IS("valueOf","(I)Ljava/lang/Integer;");
  bool ctor=IS("<init>","(I)V");
  bool value=IS("intValue","()I")||IS("byteValue","()B")||IS("shortValue","()S")||IS("longValue","()J")||IS("floatValue","()F")||IS("doubleValue","()D")||IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool compare=IS("compareTo","(Ljava/lang/Integer;)I");
  bool tostring=IS("toString","()Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!equals&&!compare&&!tostring)return false;
  if(value_of){
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Integer");dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Integer.valueOf");return true;}
   struct boxed_integer *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Integer.valueOf payload");return true;}
   box->value=a[0].i;dvm__obj(v,ref)->data=box;out->l=ref;return true;
  }
  if(ctor){struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Integer receiver");return true;}free(o->data);struct boxed_integer *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Integer payload");return true;}box->value=a[0].i;o->data=box;return true;}
  struct boxed_integer *box=boxed_integer_of(v,self);if(!box){dvm__throw(v,"java/lang/IllegalStateException","Integer not initialized");return true;}
  if(value){if(IS("intValue","()I")||IS("hashCode","()I"))out->i=box->value;else if(IS("byteValue","()B"))out->i=(int8_t)box->value;else if(IS("shortValue","()S"))out->i=(int16_t)box->value;else if(IS("longValue","()J"))out->j=box->value;else if(IS("floatValue","()F"))out->f=(float)box->value;else out->d=(double)box->value;return true;}
  if(equals){struct boxed_integer *other=boxed_integer_of(v,a[0].l);out->i=other&&other->value==box->value;return true;}
  if(compare){struct boxed_integer *other=boxed_integer_of(v,a[0].l);if(!other){dvm__throw(v,"java/lang/NullPointerException","Integer.compareTo");return true;}out->i=box->value<other->value?-1:box->value>other->value?1:0;return true;}
  char text[32];int n=snprintf(text,sizeof text,"%d",box->value);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/Long")){
  bool value_of=IS("valueOf","(J)Ljava/lang/Long;");
  bool ctor=IS("<init>","(J)V");
  bool value=IS("longValue","()J")||IS("intValue","()I")||IS("shortValue","()S")||IS("byteValue","()B")||IS("floatValue","()F")||IS("doubleValue","()D")||IS("hashCode","()I");
  bool equals=IS("equals","(Ljava/lang/Object;)Z");
  bool compare=IS("compareTo","(Ljava/lang/Long;)I");
  bool tostring=IS("toString","()Ljava/lang/String;");
  if(!value_of&&!ctor&&!value&&!equals&&!compare&&!tostring)return false;
  if(value_of){
   struct dvm_class *cls=dvm_find_class_or_external(v,"java/lang/Long");dvm_ref ref=dvm_new_object(v,cls);
   if(!ref){dvm__throw(v,"java/lang/OutOfMemoryError","Long.valueOf");return true;}
   struct boxed_long *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Long.valueOf payload");return true;}
   box->value=a[0].j;dvm__obj(v,ref)->data=box;out->l=ref;return true;
  }
  if(ctor){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Long receiver");return true;}
   free(o->data);struct boxed_long *box=calloc(1,sizeof *box);if(!box){dvm__throw(v,"java/lang/OutOfMemoryError","Long payload");return true;}box->value=a[0].j;o->data=box;return true;
  }
  struct boxed_long *box=boxed_long_of(v,self);if(!box){dvm__throw(v,"java/lang/IllegalStateException","Long not initialized");return true;}
  if(value){
   if(IS("longValue","()J"))out->j=box->value;
   else if(IS("intValue","()I"))out->i=(int32_t)box->value;
   else if(IS("hashCode","()I"))out->i=(int32_t)(box->value^(box->value>>32));
   else if(IS("shortValue","()S"))out->i=(int16_t)box->value;
   else if(IS("byteValue","()B"))out->i=(int8_t)box->value;
   else if(IS("floatValue","()F"))out->f=(float)box->value;
   else out->d=(double)box->value;
   return true;
  }
  if(equals){struct boxed_long *other=boxed_long_of(v,a[0].l);out->i=other&&other->value==box->value;return true;}
  if(compare){struct boxed_long *other=boxed_long_of(v,a[0].l);if(!other){dvm__throw(v,"java/lang/NullPointerException","Long.compareTo");return true;}out->i=box->value<other->value?-1:box->value>other->value?1:0;return true;}
  char text[32];int n=snprintf(text,sizeof text,"%" PRId64,box->value);out->l=dvm_new_string_n(v,text,(size_t)n);return true;
 }
 if(!strcmp(c,"java/lang/String")&&IS("equals","(Ljava/lang/Object;)Z")){
  struct dvm_object *left=dvm__obj(v,self),*right=dvm__obj(v,a[0].l);
  out->i=left&&right&&left->kind==DVM_OBJ_STRING&&right->kind==DVM_OBJ_STRING&&
         left->utf8_len==right->utf8_len&&!memcmp(left->utf8,right->utf8,left->utf8_len);
  return true;
 }
 if(!strcmp(c,"java/util/concurrent/atomic/AtomicBoolean")){
  bool ctor=IS("<init>","()V")||IS("<init>","(Z)V");
  if(!ctor&&!IS("get","()Z")&&!IS("set","(Z)V")&&!IS("lazySet","(Z)V")&&!IS("getAndSet","(Z)Z")&&!IS("compareAndSet","(ZZ)Z")&&!IS("weakCompareAndSet","(ZZ)Z"))return false;
  struct payload *p=payload(v,self,PAY_BOOL,ctor);if(!p)return true;
  if(ctor){atomic_store(&p->flag,IS("<init>","(Z)V")&&a[0].i);}
  else if(IS("get","()Z"))out->i=atomic_load(&p->flag);
  else if(IS("getAndSet","(Z)Z"))out->i=atomic_exchange(&p->flag,!!a[0].i);
  else if(IS("compareAndSet","(ZZ)Z")||IS("weakCompareAndSet","(ZZ)Z")){bool expected=!!a[0].i;out->i=atomic_compare_exchange_strong(&p->flag,&expected,!!a[1].i);}
  else atomic_store(&p->flag,!!a[0].i);
  return true;
 }
 if(!strcmp(c,"java/lang/ThreadLocal")){
  bool ctor=IS("<init>","()V");
  if(IS("initialValue","()Ljava/lang/Object;"))return true; /* default null */
  if(!ctor&&!IS("get","()Ljava/lang/Object;")&&!IS("set","(Ljava/lang/Object;)V")&&!IS("remove","()V"))return false;
  struct payload *p=payload(v,self,PAY_TLS,ctor);if(!p)return true;
  if(ctor)return true;
  /* Subclasses may override initialValue: an absent entry must execute it. */
  struct tls_entry **slot=&p->tls;
  while(*slot&&!pthread_equal((*slot)->thread,pthread_self()))slot=&(*slot)->next;
  if(IS("remove","()V")){if(*slot){struct tls_entry *e=*slot;*slot=e->next;free(e);}return true;}
  if(!*slot){*slot=calloc(1,sizeof **slot);if(!*slot){dvm__throw(v,"java/lang/OutOfMemoryError","ThreadLocal");return true;}(*slot)->thread=pthread_self();
   if(IS("get","()Ljava/lang/Object;")){struct dvm_object *o=dvm__obj(v,self);struct dvm_method *init=dvm_find_method(v,o->cls,"initialValue","()Ljava/lang/Object;");if(init&&(init->has_code||init->builtin)){union dvm_value r={0};if(dvm_call(v,init,self,NULL,0,&r))(*slot)->value=r.l;}}}
  if(IS("set","(Ljava/lang/Object;)V"))(*slot)->value=a[0].l;else out->l=(*slot)->value;
  return true;
 }
 struct dvm_object *receiver=self?dvm__obj(v,self):NULL;
 bool list=!strcmp(c,"java/util/concurrent/CopyOnWriteArrayList")||(receiver&&receiver->cls&&!strcmp(receiver->cls->name,"java/util/concurrent/CopyOnWriteArrayList"));
 bool iterator=receiver&&receiver->cls&&!strcmp(receiver->cls->name,"java/util/concurrent/CopyOnWriteArrayList$COWIterator");
 if(iterator){
  if(!IS("hasNext","()Z")&&!IS("next","()Ljava/lang/Object;")&&!IS("remove","()V"))return false;
  struct payload *p=payload(v,self,PAY_ITER,false);if(!p)return true;
  if(IS("hasNext","()Z"))out->i=p->pos<p->s->n;
  else if(IS("remove","()V"))dvm__throw(v,"java/lang/UnsupportedOperationException","CopyOnWrite iterator is immutable");
  else if(p->pos>=p->s->n)dvm__throw(v,"java/util/NoSuchElementException","iterator exhausted");
  else out->l=p->s->values[p->pos++];return true;
 }
 if(!list)return false;
 bool ctor=IS("<init>","()V")||IS("<init>","([Ljava/lang/Object;)V");
 if(!ctor&&!IS("size","()I")&&!IS("isEmpty","()Z")&&!IS("add","(Ljava/lang/Object;)Z")&&!IS("add","(ILjava/lang/Object;)V")&&!IS("get","(I)Ljava/lang/Object;")&&!IS("set","(ILjava/lang/Object;)Ljava/lang/Object;")&&!IS("remove","(I)Ljava/lang/Object;")&&!IS("remove","(Ljava/lang/Object;)Z")&&!IS("clear","()V")&&!IS("iterator","()Ljava/util/Iterator;")&&!IS("contains","(Ljava/lang/Object;)Z")&&!IS("indexOf","(Ljava/lang/Object;)I")&&!IS("addIfAbsent","(Ljava/lang/Object;)Z")&&!IS("toArray","()[Ljava/lang/Object;"))return false;
 struct payload *p=payload(v,self,PAY_LIST,ctor);if(!p)return true;
 if(ctor){size_t n=IS("<init>","()V")?0:dvm_array_length(v,a[0].l);if(IS("<init>","([Ljava/lang/Object;)V")&&!a[0].l){dvm__throw(v,"java/lang/NullPointerException","source array");return true;}struct snapshot *s=snapshot_new(n);if(!s)goto oom;if(n)memcpy(s->values,dvm_array_data(v,a[0].l),n*sizeof(dvm_ref));snapshot_release(p->s);p->s=s;return true;}
 if(!p->s){dvm__throw(v,"java/lang/IllegalStateException","list uninitialized");return true;}
 size_t n=p->s->n;struct snapshot *next=NULL;
 if(IS("size","()I")){out->i=(int)n;return true;}
 if(IS("isEmpty","()Z")){out->i=n==0;return true;}
 if(IS("iterator","()Ljava/util/Iterator;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/CopyOnWriteArrayList$COWIterator"));struct payload *it=payload(v,out->l,PAY_ITER,true);if(!it)return true;it->s=p->s;++p->s->owners;return true;}
 if(IS("toArray","()[Ljava/lang/Object;")){out->l=dvm_new_array(v,'L',"Ljava/lang/Object;",(uint32_t)n);if(n)memcpy(dvm_array_data(v,out->l),p->s->values,n*sizeof(dvm_ref));return true;}
 if(IS("contains","(Ljava/lang/Object;)Z")||IS("indexOf","(Ljava/lang/Object;)I")||IS("addIfAbsent","(Ljava/lang/Object;)Z")||IS("remove","(Ljava/lang/Object;)Z")){
  size_t i=0;for(;i<n;++i){if(java_basics_equal(v,a[0].l,p->s->values[i]))break;if(dvm_exception(v))return true;}
  if(IS("contains","(Ljava/lang/Object;)Z")){out->i=i<n;return true;}
  if(IS("indexOf","(Ljava/lang/Object;)I")){out->i=i<n?(int)i:-1;return true;}
  if(IS("addIfAbsent","(Ljava/lang/Object;)Z")){if(i<n)return true;next=snapshot_new(n+1);if(!next)goto oom;memcpy(next->values,p->s->values,n*sizeof(dvm_ref));next->values[n]=a[0].l;out->i=1;}
  else {if(i==n)return true;next=snapshot_new(n-1);if(!next)goto oom;memcpy(next->values,p->s->values,i*sizeof(dvm_ref));memcpy(next->values+i,p->s->values+i+1,(n-i-1)*sizeof(dvm_ref));out->i=1;}
 }else if(IS("add","(Ljava/lang/Object;)Z")||IS("add","(ILjava/lang/Object;)V")){
  int at=IS("add","(Ljava/lang/Object;)Z")?(int)n:a[0].i;if(at<0||(size_t)at>n)goto bounds;
  next=snapshot_new(n+1);if(!next)goto oom;memcpy(next->values,p->s->values,(size_t)at*sizeof(dvm_ref));next->values[at]=a[IS("add","(Ljava/lang/Object;)Z")?0:1].l;memcpy(next->values+at+1,p->s->values+at,(n-at)*sizeof(dvm_ref));out->i=1;
 }else if(IS("clear","()V")){next=snapshot_new(0);if(!next)goto oom;}
 else {int at=a[0].i;if(at<0||(size_t)at>=n)goto bounds;out->l=p->s->values[at];if(IS("get","(I)Ljava/lang/Object;"))return true;
  bool remove=IS("remove","(I)Ljava/lang/Object;");next=snapshot_new(n-remove);if(!next)goto oom;
  if(remove){memcpy(next->values,p->s->values,(size_t)at*sizeof(dvm_ref));memcpy(next->values+at,p->s->values+at+1,(n-at-1)*sizeof(dvm_ref));}else{memcpy(next->values,p->s->values,n*sizeof(dvm_ref));next->values[at]=a[1].l;}}
 snapshot_release(p->s);p->s=next;return true;
bounds:dvm__throw(v,"java/lang/IndexOutOfBoundsException","list index");return true;
oom:dvm__throw(v,"java/lang/OutOfMemoryError","base class payload");return true;
}
#include "runtime_extra.inc"
