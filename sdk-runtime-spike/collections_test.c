#include "collections.h"
#include "portable.h"
#include <stdio.h>
extern bool collections_ordered_map_selftest(struct dvm*);
static union dvm_value call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref r,union dvm_value a,union dvm_value b){
 union dvm_value args[]={a,b},out={0};if(!collections_call(v,c,m,s,r,args,2,&out))dvm__throw(v,"java/lang/AssertionError","collection test dispatch %s.%s",c,m);return out;
}
bool collections_selftest(struct dvm *v,unsigned *checks){
 bool okay=true;const char *al="java/util/ArrayList",*li="java/util/List",*itr="java/util/Iterator",*hm="java/util/HashMap",*mp="java/util/Map";union dvm_value z={0},one={.i=1},out={0};
 if(!collections_ordered_map_selftest(v))okay=false;
#define CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"SELFTEST_FAIL collections line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 dvm_ref list=dvm_new_object(v,dvm_find_class_or_external(v,al));call(v,al,"<init>","(I)V",list,one,z);CHECK(call(v,li,"isEmpty","()Z",list,z,z).i==1);
 CHECK(dvm__class_assignable(v,dvm_object_class(v,list),dvm_find_class_or_external(v,li)));CHECK(dvm__class_assignable(v,dvm_object_class(v,list),dvm_find_class_or_external(v,"java/util/Collection")));CHECK(dvm__class_assignable(v,dvm_object_class(v,list),dvm_find_class_or_external(v,"java/lang/Iterable")));
 dvm_ref values[40];for(int i=0;i<40;++i){char text[24];snprintf(text,sizeof text,"element%d",i);values[i]=dvm_new_string(v,text);union dvm_value a={.l=values[i]};CHECK(call(v,li,"add","(Ljava/lang/Object;)Z",list,a,z).i==1);}
 CHECK(call(v,li,"size","()I",list,z,z).i==40);union dvm_value last={.i=39};CHECK(call(v,li,"get","(I)Ljava/lang/Object;",list,last,z).l==values[39]);
 union dvm_value equal_string={.l=dvm_new_string(v,"element39")};CHECK(call(v,li,"indexOf","(Ljava/lang/Object;)I",list,equal_string,z).i==39);CHECK(call(v,li,"contains","(Ljava/lang/Object;)Z",list,equal_string,z).i==1);
 call(v,li,"add","(Ljava/lang/Object;)Z",list,z,z);CHECK(call(v,li,"contains","(Ljava/lang/Object;)Z",list,z,z).i==1);CHECK(call(v,li,"remove","(Ljava/lang/Object;)Z",list,z,z).i==1);CHECK(call(v,li,"size","()I",list,z,z).i==40);
 dvm_ref it=call(v,li,"iterator","()Ljava/util/Iterator;",list,z,z).l;union dvm_value replacement={.l=dvm_new_string(v,"replacement")};CHECK(call(v,li,"set","(ILjava/lang/Object;)Ljava/lang/Object;",list,z,replacement).l==values[0]);CHECK(call(v,itr,"next","()Ljava/lang/Object;",it,z,z).l==replacement.l);
 call(v,itr,"remove","()V",it,z,z);CHECK(call(v,li,"size","()I",list,z,z).i==39);CHECK(call(v,itr,"next","()Ljava/lang/Object;",it,z,z).l==values[1]);
 call(v,li,"add","(Ljava/lang/Object;)Z",list,z,z);call(v,itr,"next","()Ljava/lang/Object;",it,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 last.i=-1;call(v,li,"remove","(I)Ljava/lang/Object;",list,last,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);last.i=40;call(v,li,"set","(ILjava/lang/Object;)Ljava/lang/Object;",list,last,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);call(v,li,"get","(I)Ljava/lang/Object;",list,last,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 union dvm_value source={.l=list};dvm_ref copy=dvm_new_object(v,dvm_find_class_or_external(v,al));call(v,al,"<init>","(Ljava/util/Collection;)V",copy,source,z);CHECK(call(v,li,"size","()I",copy,z,z).i==40);call(v,li,"clear","()V",list,z,z);CHECK(call(v,li,"size","()I",copy,z,z).i==40);
 dvm_ref typed=dvm_new_array(v,'L',"Ljava/lang/Object;",44);union dvm_value arr={.l=typed};CHECK(call(v,li,"toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",copy,arr,z).l==typed);CHECK(((dvm_ref *)dvm_array_data(v,typed))[40]==0);
 dvm_ref small=dvm_new_object(v,dvm_find_class_or_external(v,al));call(v,al,"<init>","()V",small,z,z);call(v,li,"add","(Ljava/lang/Object;)Z",small,replacement,z);source.l=small;CHECK(call(v,li,"addAll","(Ljava/util/Collection;)Z",small,source,z).i==1);CHECK(call(v,li,"size","()I",small,z,z).i==2);
 it=call(v,li,"iterator","()Ljava/util/Iterator;",small,z,z).l;call(v,itr,"remove","()V",it,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);call(v,itr,"next","()Ljava/lang/Object;",it,z,z);call(v,itr,"next","()Ljava/lang/Object;",it,z,z);CHECK(call(v,itr,"hasNext","()Z",it,z,z).i==0);call(v,itr,"next","()Ljava/lang/Object;",it,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 dvm_ref negative=dvm_new_object(v,dvm_find_class_or_external(v,al));last.i=-1;call(v,al,"<init>","(I)V",negative,last,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 CHECK(!collections_call(v,al,"unimplemented","()V",small,NULL,0,&out));
 dvm_ref map=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",map,z,z);CHECK(call(v,mp,"isEmpty","()Z",map,z,z).i==1);
 union dvm_value key={.l=dvm_new_string(v,"key")},same={.l=dvm_new_string(v,"key")},a={.l=dvm_new_string(v,"a")},b={.l=dvm_new_string(v,"b")};
 CHECK(call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,key,a).l==0);CHECK(call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,same,b).l==a.l);CHECK(call(v,mp,"size","()I",map,z,z).i==1);CHECK(call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",map,key,z).l==b.l);
 call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,z,z);CHECK(call(v,mp,"containsKey","(Ljava/lang/Object;)Z",map,z,z).i==1);CHECK(call(v,mp,"getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,z,a).l==0);CHECK(call(v,mp,"getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,a,b).l==b.l);CHECK(call(v,mp,"containsValue","(Ljava/lang/Object;)Z",map,z,z).i==1);
 for(int i=0;i<40;++i){union dvm_value k={.l=values[i]};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",map,k,a);}CHECK(call(v,mp,"size","()I",map,z,z).i==42);
 CHECK(call(v,mp,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",map,same,z).l==b.l);CHECK(call(v,mp,"containsKey","(Ljava/lang/Object;)Z",map,key,z).i==0);
 source.l=map;dvm_ref mc=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","(Ljava/util/Map;)V",mc,source,z);CHECK(call(v,mp,"size","()I",mc,z,z).i==41);call(v,mp,"clear","()V",map,z,z);CHECK(call(v,mp,"size","()I",mc,z,z).i==41);CHECK(call(v,mp,"isEmpty","()Z",map,z,z).i==1);
 CHECK(!collections_call(v,hm,"entrySet","()Ljava/util/Set;",map,NULL,0,&out));CHECK(!dvm_exception(v));
 const char* linked="java/util/LinkedList";dvm_ref deque=dvm_new_object(v,dvm_find_class_or_external(v,linked));
 call(v,linked,"<init>","()V",deque,z,z);
 call(v,linked,"addLast","(Ljava/lang/Object;)V",deque,a,z);call(v,linked,"addFirst","(Ljava/lang/Object;)V",deque,b,z);
 CHECK(call(v,linked,"getFirst","()Ljava/lang/Object;",deque,z,z).l==b.l);
 CHECK(call(v,linked,"pollLast","()Ljava/lang/Object;",deque,z,z).l==a.l);
 CHECK(call(v,linked,"pop","()Ljava/lang/Object;",deque,z,z).l==b.l);
 CHECK(call(v,linked,"poll","()Ljava/lang/Object;",deque,z,z).l==0);
 call(v,linked,"pop","()Ljava/lang/Object;",deque,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 const char* ad="java/util/ArrayDeque";dvm_ref dq=dvm_new_object(v,dvm_find_class_or_external(v,ad));call(v,ad,"<init>","()V",dq,z,z);
 call(v,ad,"addFirst","(Ljava/lang/Object;)V",dq,a,z);call(v,ad,"addLast","(Ljava/lang/Object;)V",dq,b,z);
 CHECK(call(v,ad,"peek","()Ljava/lang/Object;",dq,z,z).l==a.l);CHECK(call(v,ad,"pollLast","()Ljava/lang/Object;",dq,z,z).l==b.l);CHECK(call(v,ad,"pop","()Ljava/lang/Object;",dq,z,z).l==a.l);
 call(v,ad,"poll","()Ljava/lang/Object;",dq,z,z);CHECK(!dvm_exception(v));call(v,ad,"removeFirst","()Ljava/lang/Object;",dq,z,z);CHECK(dvm_exception(v));dvm_clear_exception(v);
 call(v,ad,"add","(Ljava/lang/Object;)Z",dq,z,z);CHECK(dvm_exception(v));dvm_clear_exception(v);
 call(v,ad,"add","(Ljava/lang/Object;)Z",dq,a,z);call(v,ad,"add","(Ljava/lang/Object;)Z",dq,b,z);
 CHECK(call(v,ad,"contains","(Ljava/lang/Object;)Z",dq,b,z).i);
 dvm_ref di=call(v,ad,"iterator","()Ljava/util/Iterator;",dq,z,z).l;
 CHECK(call(v,"java/util/Iterator","next","()Ljava/lang/Object;",di,z,z).l==a.l);
 call(v,"java/util/Iterator","remove","()V",di,z,z);
 CHECK(call(v,ad,"size","()I",dq,z,z).i==1);
 CHECK(call(v,ad,"remove","(Ljava/lang/Object;)Z",dq,b,z).i);
 call(v,ad,"add","(Ljava/lang/Object;)Z",dq,a,z);call(v,ad,"clear","()V",dq,z,z);
 CHECK(call(v,ad,"isEmpty","()Z",dq,z,z).i);
 sdk_trace_fprintf(stderr,"COLLECTION_SELFTEST %s marker=collections-v6 arraydeque_deque=1 null_rejected=1 empty_poll=1 upstream_functions=25\n",okay?"PASS":"FAIL");return okay;
}
