#include "collections.h"
#include "java_basics.h"
#include "portable.h"
#include <stdio.h>
#include <string.h>
extern bool collections_ordered_map_selftest(struct dvm*);
extern bool set_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
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
 CHECK(collections_call(v,hm,"entrySet","()Ljava/util/Set;",map,NULL,0,&out)&&out.l);CHECK(!dvm_exception(v));
 /* A2: entrySet is a backed, multi-entry view whose elements are real
  * Map.Entry objects comparing by key and value. */
 const char *set="java/util/Set",*me="java/util/Map$Entry";
 dvm_ref em=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",em,z,z);
 dvm_ref es=call(v,mp,"entrySet","()Ljava/util/Set;",em,z,z).l;
 CHECK(es!=0);CHECK(dvm__class_assignable(v,dvm_object_class(v,es),dvm_find_class_or_external(v,set)));CHECK(dvm__class_assignable(v,dvm_object_class(v,es),dvm_find_class_or_external(v,"java/util/Collection")));CHECK(dvm__class_assignable(v,dvm_object_class(v,es),dvm_find_class_or_external(v,"java/lang/Iterable")));
 CHECK(call(v,set,"size","()I",es,z,z).i==0);CHECK(call(v,set,"isEmpty","()Z",es,z,z).i==1);CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",es,z,z).i==0);CHECK(call(v,set,"remove","(Ljava/lang/Object;)Z",es,z,z).i==0);CHECK(!dvm_exception(v));
 dvm_ref ei=call(v,set,"iterator","()Ljava/util/Iterator;",es,z,z).l;CHECK(call(v,itr,"hasNext","()Z",ei,z,z).i==0);call(v,itr,"next","()Ljava/lang/Object;",ei,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 union dvm_value ek1={.l=dvm_new_string(v,"alpha")},ev1={.l=dvm_new_string(v,"one")},ek2={.l=dvm_new_string(v,"beta")},ev2={.l=dvm_new_string(v,"two")},ek3={.l=dvm_new_string(v,"gamma")},ev3={.l=dvm_new_string(v,"three")};
 CHECK(call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em,ek1,ev1).l==0);
 CHECK(call(v,set,"size","()I",es,z,z).i==1);CHECK(call(v,set,"isEmpty","()Z",es,z,z).i==0);
 ei=call(v,set,"iterator","()Ljava/util/Iterator;",es,z,z).l;CHECK(call(v,itr,"hasNext","()Z",ei,z,z).i==1);
 dvm_ref e1=call(v,itr,"next","()Ljava/lang/Object;",ei,z,z).l;
 CHECK(e1!=0);CHECK(dvm__class_assignable(v,dvm_object_class(v,e1),dvm_find_class_or_external(v,me)));
 CHECK(call(v,me,"getKey","()Ljava/lang/Object;",e1,z,z).l==ek1.l);CHECK(call(v,me,"getValue","()Ljava/lang/Object;",e1,z,z).l==ev1.l);CHECK(call(v,itr,"hasNext","()Z",ei,z,z).i==0);
 call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em,ek2,ev2);call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em,ek3,ev3);
 CHECK(call(v,set,"size","()I",es,z,z).i==3);
 ei=call(v,set,"iterator","()Ljava/util/Iterator;",es,z,z).l;dvm_ref order_keys[3]={ek1.l,ek2.l,ek3.l},order_vals[3]={ev1.l,ev2.l,ev3.l};
 for(int i=0;i<3;++i){dvm_ref e=call(v,itr,"next","()Ljava/lang/Object;",ei,z,z).l;CHECK(e!=0);CHECK(call(v,me,"getKey","()Ljava/lang/Object;",e,z,z).l==order_keys[i]);CHECK(call(v,me,"getValue","()Ljava/lang/Object;",e,z,z).l==order_vals[i]);}
 CHECK(call(v,itr,"hasNext","()Z",ei,z,z).i==0);CHECK(!dvm_exception(v));
 /* Independent maps: equal-content entries compare and hash equal. */
 dvm_ref em2=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",em2,z,z);union dvm_value ek1b={.l=dvm_new_string(v,"alpha")},ev1b={.l=dvm_new_string(v,"one")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em2,ek1b,ev1b);
 dvm_ref es2=call(v,mp,"entrySet","()Ljava/util/Set;",em2,z,z).l;dvm_ref ei2=call(v,set,"iterator","()Ljava/util/Iterator;",es2,z,z).l;dvm_ref e2=call(v,itr,"next","()Ljava/lang/Object;",ei2,z,z).l;
 CHECK(e2!=0);CHECK(java_basics_equal(v,e1,e2));CHECK(!dvm_exception(v));CHECK(java_basics_equal(v,e2,e1));union dvm_value e2arg={.l=e2};CHECK(call(v,me,"equals","(Ljava/lang/Object;)Z",e1,e2arg,z).i==1);
 int h1=call(v,me,"hashCode","()I",e1,z,z).i,h2=call(v,me,"hashCode","()I",e2,z,z).i;CHECK(h1==h2);
 union dvm_value notentry={.l=dvm_new_string(v,"not-an-entry")};CHECK(call(v,me,"equals","(Ljava/lang/Object;)Z",e1,notentry,z).i==0);CHECK(!dvm_exception(v));
 CHECK(call(v,me,"toString","()Ljava/lang/String;",e1,z,z).l!=0);CHECK(!dvm_exception(v));
 /* setValue writes through; Map.put updates are visible from the entry. */
 union dvm_value updated={.l=dvm_new_string(v,"ONE-updated")};CHECK(call(v,me,"setValue","(Ljava/lang/Object;)Ljava/lang/Object;",e1,updated,z).l==ev1.l);
 CHECK(call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",em,ek1,z).l==updated.l);
 union dvm_value frommap={.l=dvm_new_string(v,"from-map")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em,ek1,frommap);
 CHECK(call(v,me,"getValue","()Ljava/lang/Object;",e1,z,z).l==frommap.l);
 /* contains/remove accept independent entries; values must match too. */
 dvm_ref em3=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",em3,z,z);union dvm_value ek1c={.l=dvm_new_string(v,"alpha")},wrong={.l=dvm_new_string(v,"wrong-value")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em3,ek1c,wrong);
 dvm_ref es3=call(v,mp,"entrySet","()Ljava/util/Set;",em3,z,z).l;dvm_ref ei3=call(v,set,"iterator","()Ljava/util/Iterator;",es3,z,z).l;dvm_ref e3=call(v,itr,"next","()Ljava/lang/Object;",ei3,z,z).l;union dvm_value e3arg={.l=e3};
 CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",es,e3arg,z).i==0);CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",es,notentry,z).i==0);CHECK(!dvm_exception(v));
 union dvm_value e2arg2={.l=e2};CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",es,e2arg2,z).i==0);
 dvm_ref em4=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",em4,z,z);union dvm_value ek1d={.l=dvm_new_string(v,"alpha")},ev1d={.l=dvm_new_string(v,"from-map")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em4,ek1d,ev1d);
 dvm_ref es4=call(v,mp,"entrySet","()Ljava/util/Set;",em4,z,z).l;dvm_ref ei4=call(v,set,"iterator","()Ljava/util/Iterator;",es4,z,z).l;dvm_ref e4=call(v,itr,"next","()Ljava/lang/Object;",ei4,z,z).l;union dvm_value e4arg={.l=e4};
 CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",es,e4arg,z).i==1);
 CHECK(call(v,set,"remove","(Ljava/lang/Object;)Z",es,e3arg,z).i==0);CHECK(call(v,mp,"size","()I",em,z,z).i==3);
 CHECK(call(v,set,"remove","(Ljava/lang/Object;)Z",es,e4arg,z).i==1);CHECK(call(v,mp,"size","()I",em,z,z).i==2);CHECK(call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",em,ek1,z).l==0);CHECK(call(v,set,"size","()I",es,z,z).i==2);
 union dvm_value after={.l=dvm_new_string(v,"after-remove")};call(v,me,"setValue","(Ljava/lang/Object;)Ljava/lang/Object;",e1,after,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 call(v,set,"add","(Ljava/lang/Object;)Z",es,notentry,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);CHECK(call(v,mp,"size","()I",em,z,z).i==2);
 call(v,set,"addAll","(Ljava/util/Collection;)Z",es,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 dvm_ref earr=call(v,set,"toArray","()[Ljava/lang/Object;",es,z,z).l;CHECK(dvm_array_length(v,earr)==2);dvm_ref *eitems=dvm_array_data(v,earr);CHECK(eitems[0]!=0);CHECK(dvm__class_assignable(v,dvm_object_class(v,eitems[0]),dvm_find_class_or_external(v,me)));
 /* Replacement put does not invalidate the iterator; structural put does. */
 ei=call(v,set,"iterator","()Ljava/util/Iterator;",es,z,z).l;union dvm_value beta_new={.l=dvm_new_string(v,"two-updated")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em,ek2,beta_new);
 CHECK(call(v,itr,"hasNext","()Z",ei,z,z).i==1);CHECK(!dvm_exception(v));dvm_ref e5=call(v,itr,"next","()Ljava/lang/Object;",ei,z,z).l;CHECK(call(v,me,"getKey","()Ljava/lang/Object;",e5,z,z).l==ek2.l);CHECK(call(v,me,"getValue","()Ljava/lang/Object;",e5,z,z).l==beta_new.l);
 union dvm_value delta={.l=dvm_new_string(v,"delta")},dval={.l=dvm_new_string(v,"four")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",em,delta,dval);CHECK(call(v,itr,"hasNext","()Z",ei,z,z).i==1);call(v,itr,"next","()Ljava/lang/Object;",ei,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 /* Iterator.remove mutates the map; repeating it without next is illegal. */
 dvm_ref rm=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",rm,z,z);union dvm_value rk1={.l=dvm_new_string(v,"r1")},rv1={.l=dvm_new_string(v,"v1")},rk2={.l=dvm_new_string(v,"r2")},rv2={.l=dvm_new_string(v,"v2")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",rm,rk1,rv1);call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",rm,rk2,rv2);
 dvm_ref rs=call(v,mp,"entrySet","()Ljava/util/Set;",rm,z,z).l;CHECK(call(v,set,"size","()I",rs,z,z).i==2);dvm_ref rorder=call(v,set,"iterator","()Ljava/util/Iterator;",rs,z,z).l;dvm_ref ro1=call(v,itr,"next","()Ljava/lang/Object;",rorder,z,z).l,ro2=call(v,itr,"next","()Ljava/lang/Object;",rorder,z,z).l;CHECK(call(v,me,"getKey","()Ljava/lang/Object;",ro1,z,z).l==rk1.l);CHECK(call(v,me,"getKey","()Ljava/lang/Object;",ro2,z,z).l==rk2.l);CHECK(call(v,itr,"hasNext","()Z",rorder,z,z).i==0);dvm_ref ri=call(v,set,"iterator","()Ljava/util/Iterator;",rs,z,z).l;dvm_ref r1=call(v,itr,"next","()Ljava/lang/Object;",ri,z,z).l;CHECK(call(v,me,"getKey","()Ljava/lang/Object;",r1,z,z).l==rk1.l);
 call(v,itr,"remove","()V",ri,z,z);CHECK(call(v,mp,"size","()I",rm,z,z).i==1);call(v,itr,"remove","()V",ri,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 dvm_ref r2=call(v,itr,"next","()Ljava/lang/Object;",ri,z,z).l;CHECK(call(v,me,"getKey","()Ljava/lang/Object;",r2,z,z).l==rk2.l);CHECK(call(v,itr,"hasNext","()Z",ri,z,z).i==0);call(v,itr,"remove","()V",ri,z,z);CHECK(call(v,mp,"size","()I",rm,z,z).i==0);CHECK(call(v,set,"size","()I",rs,z,z).i==0);
 /* clear on the view empties the backing map; null key/value works. */
 call(v,set,"clear","()V",es,z,z);CHECK(call(v,mp,"size","()I",em,z,z).i==0);CHECK(call(v,set,"size","()I",es,z,z).i==0);CHECK(call(v,set,"isEmpty","()Z",es,z,z).i==1);CHECK(!dvm_exception(v));
 dvm_ref nm=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",nm,z,z);call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",nm,z,z);dvm_ref ns=call(v,mp,"entrySet","()Ljava/util/Set;",nm,z,z).l;dvm_ref ni=call(v,set,"iterator","()Ljava/util/Iterator;",ns,z,z).l;dvm_ref ne=call(v,itr,"next","()Ljava/lang/Object;",ni,z,z).l;CHECK(ne!=0);CHECK(call(v,me,"getKey","()Ljava/lang/Object;",ne,z,z).l==0);CHECK(call(v,me,"getValue","()Ljava/lang/Object;",ne,z,z).l==0);CHECK(call(v,me,"hashCode","()I",ne,z,z).i==0);CHECK(!dvm_exception(v));
 /* A3: keySet is a backed Set over the same map, not a snapshot.  Its
  * iteration follows the map's storage order and the test therefore only
  * checks membership, never order. */
 dvm_ref km=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",km,z,z);
 union dvm_value ks_out={0};CHECK(collections_call(v,hm,"keySet","()Ljava/util/Set;",km,NULL,0,&ks_out)&&ks_out.l);
 dvm_ref ks=ks_out.l;CHECK(ks!=0);
 CHECK(dvm__class_assignable(v,dvm_object_class(v,ks),dvm_find_class_or_external(v,set)));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,ks),dvm_find_class_or_external(v,"java/util/Collection")));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,ks),dvm_find_class_or_external(v,"java/lang/Iterable")));
 CHECK(call(v,set,"size","()I",ks,z,z).i==0);CHECK(call(v,set,"isEmpty","()Z",ks,z,z).i==1);
 CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",ks,z,z).i==0);CHECK(call(v,set,"remove","(Ljava/lang/Object;)Z",ks,z,z).i==0);CHECK(!dvm_exception(v));
 dvm_ref kit=call(v,set,"iterator","()Ljava/util/Iterator;",ks,z,z).l;CHECK(kit!=0);
 CHECK(dvm__class_assignable(v,dvm_object_class(v,kit),dvm_find_class_or_external(v,itr)));
 CHECK(call(v,itr,"hasNext","()Z",kit,z,z).i==0);call(v,itr,"next","()Ljava/lang/Object;",kit,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 union dvm_value kk1={.l=dvm_new_string(v,"ks-alpha")},kv1={.l=dvm_new_string(v,"v1")};
 union dvm_value kk2={.l=dvm_new_string(v,"ks-beta")},kv2={.l=dvm_new_string(v,"v2")};
 union dvm_value kk3={.l=dvm_new_string(v,"ks-gamma")},kv3={.l=dvm_new_string(v,"v3")};
 CHECK(call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",km,kk1,kv1).l==0);
 call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",km,kk2,kv2);
 call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",km,kk3,kv3);
 CHECK(call(v,set,"size","()I",ks,z,z).i==3);CHECK(call(v,set,"isEmpty","()Z",ks,z,z).i==0);
 CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",ks,kk1,z).i==1);CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",ks,kk2,z).i==1);CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",ks,kk3,z).i==1);
 union dvm_value ks_missing={.l=dvm_new_string(v,"ks-missing")};CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",ks,ks_missing,z).i==0);CHECK(!dvm_exception(v));
 dvm_ref expected_keys[3]={kk1.l,kk2.l,kk3.l};int seen_keys[3]={0,0,0},seen_count=0;
 dvm_ref kit2=call(v,set,"iterator","()Ljava/util/Iterator;",ks,z,z).l;
 while(call(v,itr,"hasNext","()Z",kit2,z,z).i){
  dvm_ref got=call(v,itr,"next","()Ljava/lang/Object;",kit2,z,z).l;int hit=-1;
  for(int i=0;i<3;++i)if(!seen_keys[i]){bool same=java_basics_equal(v,got,expected_keys[i]);CHECK(!dvm_exception(v));if(same){hit=i;break;}}
  CHECK(hit>=0);if(hit>=0)seen_keys[hit]=1;++seen_count;
 }
 CHECK(seen_count==3);CHECK(seen_keys[0]&&seen_keys[1]&&seen_keys[2]);CHECK(call(v,itr,"hasNext","()Z",kit2,z,z).i==0);CHECK(!dvm_exception(v));
 /* keySet().remove(key) mutates the backing map and is reflected in size. */
 CHECK(call(v,set,"remove","(Ljava/lang/Object;)Z",ks,kk2,z).i==1);
 CHECK(call(v,mp,"size","()I",km,z,z).i==2);CHECK(call(v,mp,"containsKey","(Ljava/lang/Object;)Z",km,kk2,z).i==0);
 CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",ks,kk2,z).i==0);CHECK(call(v,set,"size","()I",ks,z,z).i==2);
 CHECK(call(v,set,"remove","(Ljava/lang/Object;)Z",ks,kk2,z).i==0);CHECK(!dvm_exception(v));
 /* Iterator.remove deletes the returned key from the map. */
 dvm_ref kit3=call(v,set,"iterator","()Ljava/util/Iterator;",ks,z,z).l;dvm_ref removed_key=call(v,itr,"next","()Ljava/lang/Object;",kit3,z,z).l;CHECK(removed_key!=0);union dvm_value removed_arg={.l=removed_key};
 call(v,itr,"remove","()V",kit3,z,z);CHECK(!dvm_exception(v));CHECK(call(v,mp,"size","()I",km,z,z).i==1);CHECK(call(v,mp,"containsKey","(Ljava/lang/Object;)Z",km,removed_arg,z).i==0);
 call(v,itr,"remove","()V",kit3,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 /* Structural map changes invalidate the iterator. */
 dvm_ref kit4=call(v,set,"iterator","()Ljava/util/Iterator;",ks,z,z).l;union dvm_value ks_extra={.l=dvm_new_string(v,"ks-extra")};call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",km,ks_extra,kv1);
 CHECK(call(v,itr,"hasNext","()Z",kit4,z,z).i==1);call(v,itr,"next","()Ljava/lang/Object;",kit4,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 /* add/addAll are unsupported for a keySet view. */
 union dvm_value ks_addarg={.l=dvm_new_string(v,"ks-added")},ks_addout={0};
 bool ks_add_dispatched=collections_call(v,set,"add","(Ljava/lang/Object;)Z",ks,&ks_addarg,1,&ks_addout);
 struct dvm_class *ks_add_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(ks_add_dispatched&&ks_add_exc&&!strcmp(ks_add_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 bool ks_addall_dispatched=collections_call(v,set,"addAll","(Ljava/util/Collection;)Z",ks,&ks_addarg,1,&ks_addout);
 struct dvm_class *ks_addall_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(ks_addall_dispatched&&ks_addall_exc&&!strcmp(ks_addall_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 CHECK(call(v,mp,"size","()I",km,z,z).i==2);
 /* Wrong signature and non-map receivers must not be taken over. */
 union dvm_value ks_wrong={0};CHECK(!collections_call(v,hm,"keySet","()Ljava/lang/Object;",km,NULL,0,&ks_wrong));CHECK(!dvm_exception(v));
 CHECK(!collections_call(v,"java/util/Map","keySet","()Ljava/util/Set;",small,NULL,0,&ks_wrong));CHECK(!dvm_exception(v));
 dvm_ref ks_unknown=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/Hashtable"));
 CHECK(ks_unknown!=0);CHECK(!collections_call(v,"java/util/Map","keySet","()Ljava/util/Set;",ks_unknown,NULL,0,&ks_wrong));CHECK(!dvm_exception(v));
 /* clear() on the view empties the backing map (HashMap.KeySet semantics). */
 call(v,set,"clear","()V",ks,z,z);CHECK(!dvm_exception(v));CHECK(call(v,mp,"size","()I",km,z,z).i==0);CHECK(call(v,set,"size","()I",ks,z,z).i==0);CHECK(call(v,set,"isEmpty","()Z",ks,z,z).i==1);
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
 /* Collections.emptyList(): a real immutable empty List, not a mutable
  * ArrayList stand-in.  Reads must report emptiness; add must throw. */
 union dvm_value xarg={.l=dvm_new_string(v,"x")};
 dvm_ref empty=call(v,"java/util/Collections","emptyList","()Ljava/util/List;",0,z,z).l;
 CHECK(empty!=0);CHECK(!dvm_exception(v));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,empty),dvm_find_class_or_external(v,li)));
 CHECK(call(v,li,"size","()I",empty,z,z).i==0);
 CHECK(call(v,li,"isEmpty","()Z",empty,z,z).i==1);
 CHECK(call(v,li,"contains","(Ljava/lang/Object;)Z",empty,xarg,z).i==0);
 dvm_ref empty_it=call(v,li,"iterator","()Ljava/util/Iterator;",empty,z,z).l;CHECK(empty_it!=0);
 CHECK(call(v,itr,"hasNext","()Z",empty_it,z,z).i==0);
 call(v,itr,"next","()Ljava/lang/Object;",empty_it,z,z);CHECK(dvm_exception(v)!=0);dvm_clear_exception(v);
 union dvm_value add_out={0};bool add_dispatched=collections_call(v,li,"add","(Ljava/lang/Object;)Z",empty,&xarg,1,&add_out);
 struct dvm_class *add_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(add_dispatched&&add_exc&&!strcmp(add_exc->name,"java/lang/UnsupportedOperationException"));
 dvm_clear_exception(v);CHECK(call(v,li,"size","()I",empty,z,z).i==0);
 /* Collections.emptyMap(): real immutable empty Map.  Request.Builder.<init>
  * stores it in tags and Request.Builder.tag()/Request.<init> read it through
  * Map.isEmpty()/get()/remove()/Util.immutableMap().  Insertions throw; the
  * JDK EmptyMap contract makes remove() return null and clear() a no-op. */
 union dvm_value em_key={.l=dvm_new_string(v,"em-key")},em_val={.l=dvm_new_string(v,"em-value")},em_pair[2]={em_key,em_val};
 dvm_ref empty_map=call(v,"java/util/Collections","emptyMap","()Ljava/util/Map;",0,z,z).l;
 CHECK(empty_map!=0);CHECK(!dvm_exception(v));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,empty_map),dvm_find_class_or_external(v,mp)));
 CHECK(call(v,mp,"size","()I",empty_map,z,z).i==0);
 CHECK(call(v,mp,"isEmpty","()Z",empty_map,z,z).i==1);
 CHECK(call(v,mp,"containsKey","(Ljava/lang/Object;)Z",empty_map,em_key,z).i==0);
 CHECK(call(v,mp,"containsValue","(Ljava/lang/Object;)Z",empty_map,em_val,z).i==0);
 CHECK(call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",empty_map,em_key,z).l==0);
 CHECK(call(v,mp,"get","(Ljava/lang/Object;)Ljava/lang/Object;",empty_map,z,z).l==0);
 CHECK(call(v,mp,"getOrDefault","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map,em_key,em_val).l==em_val.l);
 CHECK(call(v,mp,"hashCode","()I",empty_map,z,z).i==0);
 CHECK(call(v,mp,"toString","()Ljava/lang/String;",empty_map,z,z).l!=0);
 dvm_ref empty_hm=dvm_new_object(v,dvm_find_class_or_external(v,hm));call(v,hm,"<init>","()V",empty_hm,z,z);
 union dvm_value empty_hm_arg={.l=empty_hm};CHECK(call(v,mp,"equals","(Ljava/lang/Object;)Z",empty_map,empty_hm_arg,z).i==1);CHECK(call(v,mp,"equals","(Ljava/lang/Object;)Z",empty_map,z,z).i==0);
 union dvm_value em_out={0};
 bool em_put=collections_call(v,mp,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",empty_map,em_pair,2,&em_out);
 struct dvm_class *em_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(em_put&&em_exc&&!strcmp(em_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 em_out.l=em_val.l;
 bool em_remove=collections_call(v,mp,"remove","(Ljava/lang/Object;)Ljava/lang/Object;",empty_map,em_pair,1,&em_out);
 CHECK(em_remove&&!dvm_exception(v)&&em_out.l==0);
 bool em_clear=collections_call(v,mp,"clear","()V",empty_map,NULL,0,&em_out);
 CHECK(em_clear&&!dvm_exception(v));
 union dvm_value em_putall_arg={.l=empty_hm};
 bool em_putall=collections_call(v,mp,"putAll","(Ljava/util/Map;)V",empty_map,&em_putall_arg,1,&em_out);
 em_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(em_putall&&em_exc&&!strcmp(em_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 CHECK(call(v,mp,"size","()I",empty_map,z,z).i==0);CHECK(call(v,mp,"isEmpty","()Z",empty_map,z,z).i==1);
 /* Empty keySet/entrySet/values views iterate as empty and remain unmodifiable. */
 const char *col="java/util/Collection";
 dvm_ref em_ks=call(v,mp,"keySet","()Ljava/util/Set;",empty_map,z,z).l;
 dvm_ref em_es=call(v,mp,"entrySet","()Ljava/util/Set;",empty_map,z,z).l;
 dvm_ref em_vs=call(v,mp,"values","()Ljava/util/Collection;",empty_map,z,z).l;
 CHECK(em_ks!=0&&em_es!=0&&em_vs!=0);CHECK(!dvm_exception(v));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,em_ks),dvm_find_class_or_external(v,set)));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,em_es),dvm_find_class_or_external(v,set)));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,em_vs),dvm_find_class_or_external(v,col)));
 CHECK(call(v,set,"size","()I",em_ks,z,z).i==0);CHECK(call(v,set,"size","()I",em_es,z,z).i==0);
 CHECK(call(v,col,"size","()I",em_vs,z,z).i==0);
 dvm_ref em_ks_it=call(v,set,"iterator","()Ljava/util/Iterator;",em_ks,z,z).l;
 dvm_ref em_es_it=call(v,set,"iterator","()Ljava/util/Iterator;",em_es,z,z).l;
 dvm_ref em_vs_it=call(v,col,"iterator","()Ljava/util/Iterator;",em_vs,z,z).l;
 CHECK(em_ks_it!=0&&em_es_it!=0&&em_vs_it!=0);
 CHECK(call(v,itr,"hasNext","()Z",em_ks_it,z,z).i==0);CHECK(call(v,itr,"hasNext","()Z",em_es_it,z,z).i==0);CHECK(call(v,itr,"hasNext","()Z",em_vs_it,z,z).i==0);CHECK(!dvm_exception(v));
 /* Collections.emptySet(): real immutable empty Set. */
 dvm_ref empty_set=call(v,"java/util/Collections","emptySet","()Ljava/util/Set;",0,z,z).l;
 CHECK(empty_set!=0);CHECK(!dvm_exception(v));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,empty_set),dvm_find_class_or_external(v,set)));
 CHECK(dvm__class_assignable(v,dvm_object_class(v,empty_set),dvm_find_class_or_external(v,col)));
 CHECK(call(v,set,"size","()I",empty_set,z,z).i==0);
 CHECK(call(v,set,"isEmpty","()Z",empty_set,z,z).i==1);
 CHECK(call(v,set,"contains","(Ljava/lang/Object;)Z",empty_set,em_key,z).i==0);
 /* JDK EmptySet: containsAll(c) is c.isEmpty(); null is an NPE. */
 union dvm_value em_ks_arg={.l=em_ks};
 CHECK(call(v,set,"containsAll","(Ljava/util/Collection;)Z",empty_set,em_ks_arg,z).i==1);
 union dvm_value small_arg={.l=small};
 CHECK(call(v,set,"containsAll","(Ljava/util/Collection;)Z",empty_set,small_arg,z).i==0);
 CHECK(!dvm_exception(v));
 call(v,set,"containsAll","(Ljava/util/Collection;)Z",empty_set,z,z);
 struct dvm_class *es_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(es_exc&&!strcmp(es_exc->name,"java/lang/NullPointerException"));dvm_clear_exception(v);
 dvm_ref empty_set_it=call(v,set,"iterator","()Ljava/util/Iterator;",empty_set,z,z).l;
 CHECK(empty_set_it!=0);CHECK(call(v,itr,"hasNext","()Z",empty_set_it,z,z).i==0);
 union dvm_value es_add_out={0};
 bool es_add=collections_call(v,set,"add","(Ljava/lang/Object;)Z",empty_set,&em_key,1,&es_add_out);
 es_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(es_add&&es_exc&&!strcmp(es_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 es_add_out.i=1;
 bool es_remove=collections_call(v,set,"remove","(Ljava/lang/Object;)Z",empty_set,&em_key,1,&es_add_out);
 CHECK(es_remove&&!dvm_exception(v)&&es_add_out.i==0);
 es_add_out.i=1;
 bool es_removeall=collections_call(v,set,"removeAll","(Ljava/util/Collection;)Z",empty_set,&em_ks_arg,1,&es_add_out);
 CHECK(es_removeall&&!dvm_exception(v)&&es_add_out.i==0);
 es_add_out.i=1;
 bool es_retainall=collections_call(v,set,"retainAll","(Ljava/util/Collection;)Z",empty_set,&em_ks_arg,1,&es_add_out);
 CHECK(es_retainall&&!dvm_exception(v)&&es_add_out.i==0);
 bool es_clear=collections_call(v,set,"clear","()V",empty_set,NULL,0,&es_add_out);
 CHECK(es_clear&&!dvm_exception(v));
 CHECK(call(v,set,"size","()I",empty_set,z,z).i==0);CHECK(call(v,set,"isEmpty","()Z",empty_set,z,z).i==1);
 /* Collections.addAll(Collection,Object[]): okhttp3/Headers.newBuilder()
  * passes a fresh ArrayList and the Headers String[] backing array, then
  * discards the boolean.  Per Java, add() runs once per slot in array order,
  * and the result is true iff at least one call changed the collection. */
 const char *addall_sig="(Ljava/util/Collection;[Ljava/lang/Object;)Z";
 dvm_ref addall_list=dvm_new_object(v,dvm_find_class_or_external(v,al));
 call(v,al,"<init>","()V",addall_list,z,z);
 dvm_ref addall_values[3]={dvm_new_string(v,"h-one"),dvm_new_string(v,"h-two"),dvm_new_string(v,"h-three")};
 dvm_ref addall_array=dvm_new_array(v,'L',"Ljava/lang/String;",3);
 dvm_ref *addall_slots=dvm_array_data(v,addall_array);
 for(int i=0;i<3;++i)addall_slots[i]=addall_values[i];
 union dvm_value addall_args[2]={{.l=addall_list},{.l=addall_array}},addall_out={0};
 CHECK(collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_args,2,&addall_out)&&!dvm_exception(v)&&addall_out.i==1);
 CHECK(call(v,li,"size","()I",addall_list,z,z).i==3);
 for(int i=0;i<3;++i){union dvm_value addall_index={.i=i};CHECK(call(v,li,"get","(I)Ljava/lang/Object;",addall_list,addall_index,z).l==addall_values[i]);}
 /* Empty array: false, and no element is added. */
 dvm_ref addall_none=dvm_new_array(v,'L',"Ljava/lang/Object;",0);
 union dvm_value addall_empty_args[2]={{.l=addall_list},{.l=addall_none}};
 addall_out.i=1;
 CHECK(collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_empty_args,2,&addall_out)&&!dvm_exception(v)&&addall_out.i==0);
 CHECK(call(v,li,"size","()I",addall_list,z,z).i==3);
 /* Set semantics: the repeated element's add() returns false but the mixed
  * call still reports true; an all-duplicate call reports false and changes
  * nothing.  Order follows the array for a LinkedHashSet. */
 const char *addall_set_name="java/util/LinkedHashSet";
 dvm_ref addall_set=dvm_new_object(v,dvm_find_class_or_external(v,addall_set_name));
 union dvm_value addall_set_out={0};
 CHECK(set_runtime_call(v,addall_set_name,"<init>","()V",addall_set,NULL,0,&addall_set_out)&&!dvm_exception(v));
 dvm_ref addall_set_values[3]={addall_values[0],addall_values[1],addall_values[0]};
 dvm_ref addall_set_array=dvm_new_array(v,'L',"Ljava/lang/String;",3);
 dvm_ref *addall_set_slots=dvm_array_data(v,addall_set_array);
 for(int i=0;i<3;++i)addall_set_slots[i]=addall_set_values[i];
 union dvm_value addall_set_args[2]={{.l=addall_set},{.l=addall_set_array}};
 CHECK(collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_set_args,2,&addall_out)&&!dvm_exception(v)&&addall_out.i==1);
 CHECK(set_runtime_call(v,"java/util/Set","size","()I",addall_set,NULL,0,&addall_set_out)&&addall_set_out.i==2);
 CHECK(set_runtime_call(v,"java/util/Set","iterator","()Ljava/util/Iterator;",addall_set,NULL,0,&addall_set_out)&&addall_set_out.l);
 dvm_ref addall_set_it=addall_set_out.l;
 CHECK(set_runtime_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",addall_set_it,NULL,0,&addall_set_out)&&addall_set_out.l==addall_values[0]);
 CHECK(set_runtime_call(v,"java/util/Iterator","next","()Ljava/lang/Object;",addall_set_it,NULL,0,&addall_set_out)&&addall_set_out.l==addall_values[1]);
 CHECK(set_runtime_call(v,"java/util/Iterator","hasNext","()Z",addall_set_it,NULL,0,&addall_set_out)&&!addall_set_out.i);
 addall_out.i=1;
 CHECK(collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_set_args,2,&addall_out)&&!dvm_exception(v)&&addall_out.i==0);
 CHECK(set_runtime_call(v,"java/util/Set","size","()I",addall_set,NULL,0,&addall_set_out)&&addall_set_out.i==2);
 /* Null collection and null array are NullPointerException; neither call
  * may mutate the valid collection. */
 union dvm_value addall_null_collection[2]={{.l=0},{.l=addall_array}},addall_null_out={0};
 bool addall_null_collection_ok=collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_null_collection,2,&addall_null_out);
 struct dvm_class *addall_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(addall_null_collection_ok&&addall_exc&&!strcmp(addall_exc->name,"java/lang/NullPointerException"));dvm_clear_exception(v);
 union dvm_value addall_null_elements[2]={{.l=addall_list},{.l=0}};
 bool addall_null_elements_ok=collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_null_elements,2,&addall_null_out);
 addall_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(addall_null_elements_ok&&addall_exc&&!strcmp(addall_exc->name,"java/lang/NullPointerException"));dvm_clear_exception(v);
 CHECK(call(v,li,"size","()I",addall_list,z,z).i==3);
 /* Immutable targets propagate UnsupportedOperationException unchanged:
  * Collections.emptyList(), Collections.emptySet(), and emptyMap().keySet(). */
 union dvm_value addall_ro_args[2]={{.l=empty},{.l=addall_array}};
 bool addall_empty_list_ok=collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_ro_args,2,&addall_out);
 addall_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(addall_empty_list_ok&&addall_exc&&!strcmp(addall_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 CHECK(call(v,li,"size","()I",empty,z,z).i==0);
 union dvm_value addall_set_ro_args[2]={{.l=empty_set},{.l=addall_array}};
 bool addall_empty_set_ok=collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_set_ro_args,2,&addall_out);
 addall_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(addall_empty_set_ok&&addall_exc&&!strcmp(addall_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 CHECK(call(v,set,"size","()I",empty_set,z,z).i==0);
 union dvm_value addall_emks_args[2]={{.l=em_ks},{.l=addall_array}};
 bool addall_emks_ok=collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_emks_args,2,&addall_out);
 addall_exc=dvm_exception(v)?dvm_object_class(v,dvm_exception(v)):NULL;
 CHECK(addall_emks_ok&&addall_exc&&!strcmp(addall_exc->name,"java/lang/UnsupportedOperationException"));dvm_clear_exception(v);
 CHECK(call(v,set,"size","()I",em_ks,z,z).i==0);
 /* Wrong signature/arity and unknown receivers stay fail-closed: not
  * dispatched, no exception, and the caller's array is untouched. */
 union dvm_value addall_wrong_out={0};
 CHECK(!collections_call(v,"java/util/Collections","addAll","(Ljava/util/List;[Ljava/lang/Object;)Z",0,addall_args,2,&addall_wrong_out));CHECK(!dvm_exception(v));
 CHECK(!collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_args,1,&addall_wrong_out));CHECK(!dvm_exception(v));
 union dvm_value addall_bad_elements[2]={{.l=addall_list},{.l=addall_values[0]}};
 CHECK(!collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_bad_elements,2,&addall_wrong_out));CHECK(!dvm_exception(v));
 dvm_ref addall_unknown=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/Hashtable"));
 union dvm_value addall_unknown_args[2]={{.l=addall_unknown},{.l=addall_array}};
 CHECK(!collections_call(v,"java/util/Collections","addAll",addall_sig,0,addall_unknown_args,2,&addall_wrong_out));CHECK(!dvm_exception(v));
 CHECK(call(v,li,"size","()I",addall_list,z,z).i==3);
 /* Exact signatures only: lookalikes and unknown methods stay fail-closed. */
 union dvm_value wrong_out={0};
 CHECK(!collections_call(v,"java/util/Collections","emptyMap","()Ljava/lang/Object;",0,NULL,0,&wrong_out));CHECK(!dvm_exception(v));
 CHECK(!collections_call(v,"java/util/Collections","emptySet","()Ljava/util/Collection;",0,NULL,0,&wrong_out));CHECK(!dvm_exception(v));
 CHECK(!collections_call(v,mp,"unimplemented","()V",empty_map,NULL,0,&wrong_out));CHECK(!dvm_exception(v));
 CHECK(!collections_call(v,set,"unimplemented","()V",empty_set,NULL,0,&wrong_out));CHECK(!dvm_exception(v));
 sdk_trace_fprintf(stderr,"COLLECTION_SELFTEST %s marker=collections-v7 arraydeque_deque=1 null_rejected=1 empty_poll=1 upstream_functions=25 entryset_multi=1 entryset_backed=1 entryset_entry_equals=1 entryset_cme=1 keyset_backed=1 keyset_iter=1 keyset_remove=1 keyset_uoe=1 emptymap_immutable=1 emptyset_immutable=1 empty_views=1 emptymap_remove_null=1 emptymap_clear_noop=1 emptyset_remove_false=1 emptyset_bulk_noop=1 emptyset_containsall_jdk=1 addall_list_order=1 addall_set_semantics=1 addall_empty_false=1 addall_npe=1 addall_uoe=1 addall_failclosed=1\n",okay?"PASS":"FAIL");return okay;
}
