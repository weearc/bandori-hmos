#include "numeric_runtime.h"
#include "java_basics.h"
#include "portable.h"
bool numeric_runtime_selftest(struct dvm *v){
 unsigned checks=0;bool ok=true;
 const struct{const char *text;int radix;bool valid;int32_t value;} boxed[]={{"2147483647",10,true,INT32_MAX},{"-80000000",16,true,INT32_MIN},{"2147483648",10,false,0},{" 1",10,false,0},{NULL,10,false,0}};
 for(size_t i=0;i<sizeof boxed/sizeof boxed[0];i++){
  union dvm_value args[]={{.l=boxed[i].text?dvm_new_string(v,boxed[i].text):0},{.i=boxed[i].radix}},box={0},value={0};bool radix=boxed[i].radix!=10;
  bool handled=java_basics_call(v,"java/lang/Integer","valueOf",radix?"(Ljava/lang/String;I)Ljava/lang/Integer;":"(Ljava/lang/String;)Ljava/lang/Integer;",0,args,radix?2:1,&box);
  ++checks;if(boxed[i].valid)ok=ok&&handled&&!dvm_exception(v)&&box.l&&java_basics_call(v,"java/lang/Integer","intValue","()I",box.l,NULL,0,&value)&&value.i==boxed[i].value;else ok=ok&&handled&&dvm_exception(v);dvm_clear_exception(v);
 }
 const struct{const char *text;int base;bool wide,valid;int64_t expected;} cases[]={
 {"0",10,false,true,0},{"-0",10,false,true,0},{"+1",10,false,true,1},{"2147483647",10,false,true,INT32_MAX},{"-2147483648",10,false,true,INT32_MIN},{"2147483648",10,false,false,0},{"-2147483649",10,false,false,0},
 {"9223372036854775807",10,true,true,INT64_MAX},{"-9223372036854775808",10,true,true,INT64_MIN},{"9223372036854775808",10,true,false,0},{"-9223372036854775809",10,true,false,0},
 {"7fffffff",16,false,true,INT32_MAX},{"-80000000",16,false,true,INT32_MIN},{"FFFFFFFF",16,false,false,0},{"10101",2,false,true,21},{"z",36,false,true,35},
 {"1",1,false,false,0},{"1",37,false,false,0},{"",10,false,false,0},{"+",10,false,false,0},{"-",10,false,false,0},{" 1",10,false,false,0},{"1 ",10,false,false,0},{"1x",10,false,false,0},{"0x10",16,false,false,0},{"1_0",10,false,false,0},{NULL,10,false,false,0},
 {"\xd9\xa1\xd9\xa2",10,false,true,12},{"\xef\xbc\x91\xef\xbc\x92",10,false,true,12},{"\xef\xbc\xa6",16,false,true,15},{"\xf0\x9d\x9f\x99",10,false,false,0}};
 for(size_t i=0;i<sizeof cases/sizeof cases[0];++i){union dvm_value a[]={{.l=cases[i].text?dvm_new_string(v,cases[i].text):0},{.i=cases[i].base}},out={0};bool handled=numeric_runtime_call(v,cases[i].wide?"java/lang/Long":"java/lang/Integer",cases[i].wide?"parseLong":"parseInt",cases[i].wide?"(Ljava/lang/String;I)J":"(Ljava/lang/String;I)I",0,a,2,&out);bool exception=dvm_exception(v)!=0;bool pass=handled&&(cases[i].valid?(!exception&&(cases[i].wide?out.j:(int64_t)out.i)==cases[i].expected):exception);++checks;if(!pass){ok=false;sdk_trace_fprintf(stderr,"NUMERIC_TEST_FAIL case=%zu\n",i);}dvm_clear_exception(v);}
 const struct{const char *text;bool wide;int64_t expected;} decimal[]={{"2147483647",false,INT32_MAX},{"-9223372036854775808",true,INT64_MIN}};
 for(size_t i=0;i<sizeof decimal/sizeof decimal[0];++i){union dvm_value a={.l=dvm_new_string(v,decimal[i].text)},out={0};bool handled=numeric_runtime_call(v,decimal[i].wide?"java/lang/Long":"java/lang/Integer",decimal[i].wide?"parseLong":"parseInt",decimal[i].wide?"(Ljava/lang/String;)J":"(Ljava/lang/String;)I",0,&a,1,&out);++checks;ok=ok&&handled&&!dvm_exception(v)&&(decimal[i].wide?out.j:(int64_t)out.i)==decimal[i].expected;dvm_clear_exception(v);}
 const char malformed[][3]={{(char)0xc0,(char)0xaf,0},{(char)0xed,(char)0xa0,(char)0x80}};
 for(size_t i=0;i<sizeof malformed/sizeof malformed[0];++i){union dvm_value a={.l=dvm_new_string_n(v,malformed[i],2)},radix={.i=10},args[]={a,radix},out={0};bool handled=numeric_runtime_call(v,"java/lang/Integer","parseInt","(Ljava/lang/String;I)I",0,args,2,&out);++checks;ok=ok&&handled&&dvm_exception(v);dvm_clear_exception(v);}
 const char *bools[]={NULL,"true","TRUE","TrUe","false"," true","truex"};for(size_t i=0;i<sizeof bools/sizeof bools[0];++i){union dvm_value a={.l=bools[i]?dvm_new_string(v,bools[i]):0},out={0};bool handled=numeric_runtime_call(v,"java/lang/Boolean","parseBoolean","(Ljava/lang/String;)Z",0,&a,1,&out);++checks;ok=ok&&handled&&out.i==(i>=1&&i<=3)&&!dvm_exception(v);}
 sdk_trace_fprintf(stderr,"NUMERIC_SELFTEST %s marker=numeric-batch-v10 checks=%u strict_overflow=1 invalid_tail=1 radix=1 BMP_digits=1\n",ok?"PASS":"FAIL",checks);return ok;
}
