/* Local notification values and channel registry. No OS notification delivery.
 * All stored references are ordinary fields/arrays visible to the GC. */
extern bool collections_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool notification_services_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool ns_builtin(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *o){
 struct dvm_method *m=dvm__builtin_method();
 return m&&notification_services_call(v,m->cls->name,m->name,m->sig,self,a,n,o);
}
typedef struct {const char *field,*type,*get,*set;} ns_property;
static const ns_property ns_properties[]={
 {"id","Ljava/lang/String;","getId",NULL},
 {"name","Ljava/lang/CharSequence;","getName","setName"},
 {"importance","I","getImportance","setImportance"},
 {"description","Ljava/lang/String;","getDescription","setDescription"},
 {"lights","Z","shouldShowLights","enableLights"},
 {"vibration","Z","shouldVibrate","enableVibration"},
 {"lightColor","I","getLightColor","setLightColor"},
 {"lockscreenVisibility","I","getLockscreenVisibility","setLockscreenVisibility"},
 {"sound","Landroid/net/Uri;","getSound",NULL},
 {"audioAttributes","Landroid/media/AudioAttributes;","getAudioAttributes",NULL},
 {"vibrationPattern","[J","getVibrationPattern","setVibrationPattern"},
 {"group","Ljava/lang/String;","getGroup","setGroup"},
 {"showBadge","Z","canShowBadge","setShowBadge"},
 {"bypassDnd","Z","canBypassDnd","setBypassDnd"}
};
static bool ns_add_method(struct dvm *v,struct dvm_class *k,const char *name,const char *sig){
 struct dvm_method *p=realloc(k->methods,(k->nmethods+1)*sizeof *p);
 if(!p)return oom(v,"notification method table");k->methods=p;
 struct dvm_method *m=&p[k->nmethods++];memset(m,0,sizeof *m);
 m->cls=k;m->name=name;m->sig=strdup(sig);m->access=DEX_ACC_PUBLIC;
 m->arg_slots=dvm_sig_arg_slots(sig);m->arg_count=dvm_sig_arg_count(sig);m->ret_kind=dvm_sig_return_kind(sig);m->builtin=ns_builtin;
 return m->sig!=NULL||oom(v,"notification signature");
}
static bool ns_add_fields(struct dvm *v,struct dvm_class *k,const ns_property *p,unsigned count){
 k->ifields=calloc(count,sizeof *k->ifields);if(!k->ifields)return oom(v,"notification fields");
 unsigned slot=k->super?k->super->islots:0;k->nifields=count;
 for(unsigned i=0;i<count;i++)k->ifields[i]=(struct dvm_field){.cls=k,.name=p[i].field,.type=p[i].type,.slot=slot++,.width=1,.kind=dvm__kind_of(p[i].type),.access=DEX_ACC_PRIVATE};
 k->islots=slot;return true;
}
static bool notification_services_install(struct dvm *v){
 struct dvm_class *k=dvm_find_class_or_external(v,"android/app/NotificationChannel");
 if(!k)return false;
 if(k->external&&!k->nmethods){
  if(!ns_add_fields(v,k,ns_properties,sizeof ns_properties/sizeof ns_properties[0]))return false;
  if(!ns_add_method(v,k,"<init>","(Ljava/lang/String;Ljava/lang/CharSequence;I)V"))return false;
  for(unsigned i=0;i<sizeof ns_properties/sizeof ns_properties[0];i++){
   char sig[128];snprintf(sig,sizeof sig,"()%s",ns_properties[i].type);
   if(!ns_add_method(v,k,ns_properties[i].get,sig))return false;
   if(ns_properties[i].set){snprintf(sig,sizeof sig,"(%s)V",ns_properties[i].type);if(!ns_add_method(v,k,ns_properties[i].set,sig))return false;}
  }
  if(!ns_add_method(v,k,"setSound","(Landroid/net/Uri;Landroid/media/AudioAttributes;)V"))return false;
 }
 k=dvm_find_class_or_external(v,"android/app/NotificationManager");if(!k)return false;
 if(k->external&&!k->nmethods){
  static const ns_property f[]={ {"channels","[Landroid/app/NotificationChannel;",NULL,NULL} };
  static const struct {const char *m,*s;} methods[]={
   {"createNotificationChannel","(Landroid/app/NotificationChannel;)V"},
   {"createNotificationChannels","(Ljava/util/List;)V"},
   {"getNotificationChannel","(Ljava/lang/String;)Landroid/app/NotificationChannel;"},
   {"deleteNotificationChannel","(Ljava/lang/String;)V"},
   {"getNotificationChannels","()Ljava/util/List;"}, {"areNotificationsEnabled","()Z"},
   {"notify","(ILandroid/app/Notification;)V"},{"notify","(Ljava/lang/String;ILandroid/app/Notification;)V"},
   {"cancel","(I)V"},{"cancel","(Ljava/lang/String;I)V"},{"cancelAll","()V"}
  };
  if(!ns_add_fields(v,k,f,1))return false;
  for(unsigned i=0;i<sizeof methods/sizeof methods[0];i++)if(!ns_add_method(v,k,methods[i].m,methods[i].s))return false;
 }
 k=dvm_find_class_or_external(v,"android/os/Vibrator");if(!k)return false;
 if(k->external&&!k->nmethods){
  if(!ns_add_method(v,k,"hasVibrator","()Z")||!ns_add_method(v,k,"hasAmplitudeControl","()Z")||!ns_add_method(v,k,"cancel","()V")||!ns_add_method(v,k,"vibrate","(J)V")||!ns_add_method(v,k,"vibrate","([JI)V"))return false;
 }
 return !dvm_exception(v);
}
static dvm_ref ns_pattern_copy(struct dvm *v,dvm_ref src){
 if(!src)return 0;uint32_t n=dvm_array_length(v,src);
 dvm_ref copy=dvm_new_array(v,'J',"J",n);if(copy)memcpy(dvm_array_data(v,copy),dvm_array_data(v,src),(size_t)n*sizeof(int64_t));return copy;
}
static bool ns_channel_change(struct dvm *v,dvm_ref self,dvm_ref channel,dvm_ref id,bool remove){
 union dvm_value old={0},value={0};
 if(!dvm_get_field(v,self,"channels","[Landroid/app/NotificationChannel;",&old))return false;
 if(channel&&!dvm_get_field(v,channel,"id","Ljava/lang/String;",&value))return false;
 if(channel)id=value.l;
 if(!id){dvm__throw(v,"java/lang/NullPointerException","notification channel id");return true;}
 const char *key=dvm_string_utf8(v,id);uint32_t n=old.l?dvm_array_length(v,old.l):0,index=n;
 dvm_ref *items=old.l?dvm_array_data(v,old.l):NULL;
 for(uint32_t i=0;i<n;i++){
  if(!dvm_get_field(v,items[i],"id","Ljava/lang/String;",&value))return false;
  const char *other=dvm_string_utf8(v,value.l);if(key&&other&&!strcmp(key,other)){index=i;break;}
 }
 if(remove&&index==n)return true;
 uint32_t count=remove?n-1:n+(index==n);
 dvm_ref arr=dvm_new_array(v,'L',"Landroid/app/NotificationChannel;",count);if(!arr)return false;
 dvm_ref *dst=dvm_array_data(v,arr);uint32_t j=0;
 for(uint32_t i=0;i<n;i++){if(remove&&i==index)continue;dst[j++]=i==index?channel:items[i];}
 if(!remove&&index==n)dst[j]=channel;
 return dvm_set_field(v,self,"channels","[Landroid/app/NotificationChannel;",(union dvm_value){.l=arr});
}
static bool notification_services_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 bool channel=!strcmp(c,"android/app/NotificationChannel"),manager=!strcmp(c,"android/app/NotificationManager"),vibrator=!strcmp(c,"android/os/Vibrator");
 if(!channel&&!manager&&!vibrator)return false;
 if(!self){dvm__throw(v,"java/lang/NullPointerException","notification service receiver");return true;}
 memset(out,0,sizeof *out);
 if(channel){
  if(IS("<init>","(Ljava/lang/String;Ljava/lang/CharSequence;I)V")&&n==3){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","channel id");return true;}
   return dvm_set_field(v,self,"id","Ljava/lang/String;",a[0])&&dvm_set_field(v,self,"name","Ljava/lang/CharSequence;",a[1])&&dvm_set_field(v,self,"importance","I",a[2])&&dvm_set_field(v,self,"showBadge","Z",(union dvm_value){.i=1})&&dvm_set_field(v,self,"lockscreenVisibility","I",(union dvm_value){.i=-1000});
  }
  if(IS("setSound","(Landroid/net/Uri;Landroid/media/AudioAttributes;)V")&&n==2)return dvm_set_field(v,self,"sound","Landroid/net/Uri;",a[0])&&dvm_set_field(v,self,"audioAttributes","Landroid/media/AudioAttributes;",a[1]);
  for(unsigned i=0;i<sizeof ns_properties/sizeof ns_properties[0];i++){
   const ns_property *p=&ns_properties[i];char expected[128];snprintf(expected,sizeof expected,"()%s",p->type);
   if(!strcmp(m,p->get)&&!strcmp(sig,expected)&&n==0){
    if(!dvm_get_field(v,self,p->field,p->type,out))return false;
    if(!strcmp(p->field,"vibrationPattern")&&out->l)out->l=ns_pattern_copy(v,out->l);
    return !dvm_exception(v);
   }
   snprintf(expected,sizeof expected,"(%s)V",p->type);
   if(p->set&&!strcmp(m,p->set)&&!strcmp(sig,expected)&&n==1){
    union dvm_value val=a[0];if(!strcmp(p->field,"vibrationPattern")){
     val.l=ns_pattern_copy(v,val.l);if(dvm_exception(v))return true;
     if(!dvm_set_field(v,self,"vibration","Z",(union dvm_value){.i=val.l&&dvm_array_length(v,val.l)>0}))return false;
    }
    return dvm_set_field(v,self,p->field,p->type,val);
   }
  }
  return false;
 }
 if(vibrator){
  if((IS("hasVibrator","()Z")||IS("hasAmplitudeControl","()Z")||IS("cancel","()V"))&&n==0)return true;
  if((IS("vibrate","(J)V")&&n==1)||(IS("vibrate","([JI)V")&&n==2))return true;
  return false;
 }
 if(IS("areNotificationsEnabled","()Z")&&n==0){out->i=1;return true;}
 if(IS("createNotificationChannel","(Landroid/app/NotificationChannel;)V")&&n==1){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","channel");return true;}
  return ns_channel_change(v,self,a[0].l,0,false);
 }
 if(IS("deleteNotificationChannel","(Ljava/lang/String;)V")&&n==1)return ns_channel_change(v,self,0,a[0].l,true);
 if(IS("createNotificationChannels","(Ljava/util/List;)V")&&n==1){
  union dvm_value size={0},item={0};
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","channels");return true;}
  if(!collections_call(v,"java/util/List","size","()I",a[0].l,NULL,0,&size))return false;
  for(int i=0;i<size.i;i++){union dvm_value index={.i=i};if(!collections_call(v,"java/util/List","get","(I)Ljava/lang/Object;",a[0].l,&index,1,&item)||dvm_exception(v))return true;if(!item.l){dvm__throw(v,"java/lang/NullPointerException","channel");return true;}if(!ns_channel_change(v,self,item.l,0,false))return false;}
  return true;
 }
 if(IS("getNotificationChannel","(Ljava/lang/String;)Landroid/app/NotificationChannel;")&&n==1){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","channel id");return true;}
  union dvm_value arr={0},id={0};if(!dvm_get_field(v,self,"channels","[Landroid/app/NotificationChannel;",&arr))return false;
  const char *key=dvm_string_utf8(v,a[0].l);uint32_t count=arr.l?dvm_array_length(v,arr.l):0;dvm_ref *items=arr.l?dvm_array_data(v,arr.l):NULL;
  for(uint32_t i=0;i<count;i++){if(!dvm_get_field(v,items[i],"id","Ljava/lang/String;",&id))return false;const char *s=dvm_string_utf8(v,id.l);if(key&&s&&!strcmp(key,s)){out->l=items[i];break;}}
  return true;
 }
 if(IS("getNotificationChannels","()Ljava/util/List;")&&n==0){
  dvm_ref list=new_plain_object(v,"java/util/ArrayList","notification channel list");union dvm_value ignored={0},arr={0};if(!list)return false;
  if(!collections_call(v,"java/util/ArrayList","<init>","()V",list,NULL,0,&ignored)||!dvm_get_field(v,self,"channels","[Landroid/app/NotificationChannel;",&arr))return false;
  uint32_t count=arr.l?dvm_array_length(v,arr.l):0;dvm_ref *items=arr.l?dvm_array_data(v,arr.l):NULL;
  for(uint32_t i=0;i<count;i++){union dvm_value val={.l=items[i]};if(!collections_call(v,"java/util/ArrayList","add","(Ljava/lang/Object;)Z",list,&val,1,&ignored)||dvm_exception(v))return true;}
  out->l=list;return true;
 }
 if((IS("notify","(ILandroid/app/Notification;)V")&&n==2)||(IS("notify","(Ljava/lang/String;ILandroid/app/Notification;)V")&&n==3)||(IS("cancel","(I)V")&&n==1)||(IS("cancel","(Ljava/lang/String;I)V")&&n==2)||(IS("cancelAll","()V")&&n==0)){
  sdk_trace_fprintf(stderr,"NOTIFICATION_LOCAL method=%s system_delivery=0\n",m);return true;
 }
 return false;
}

static bool ns_test_call(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *obj=dvm__obj(v,self);struct dvm_method *m=obj?dvm_find_method(v,obj->cls,name,sig):NULL;
 return m&&dvm_call(v,m,self,a,n,out)&&!dvm_exception(v);
}
bool sys_service_selftest(struct dvm *v){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/sys-service-selftest.txt","a");if(!f)return false;
 unsigned checks=0;bool ok=false;const char *stage="start";
#define NS_CHECK(label,expr) do{stage=label;bool pass=(expr)&&!dvm_exception(v);checks++;fprintf(f,"pid=%d %s %s\n",getpid(),pass?"PASS":"FAIL",label);if(!pass)goto done;}while(0)
 struct dvm_class *ck=dvm_find_class_or_external(v,"android/content/Context");dvm_ref ctx=dvm_new_object(v,ck);union dvm_value a[3]={{0}},out={0};
 static const struct {const char *name,*type;} services[]={
  {"notification","android/app/NotificationManager"},{"activity","android/app/ActivityManager"},
  {"window","android/view/WindowManager"},{"layout_inflater","android/view/LayoutInflater"},
  {"vibrator","android/os/Vibrator"},{"connectivity","android/net/ConnectivityManager"},
  {"audio","android/media/AudioManager"},{"phone","android/telephony/TelephonyManager"},
  {"clipboard","android/content/ClipboardManager"},{"media_router","android/media/MediaRouter"},
  {"input","android/hardware/input/InputManager"}
 };
 dvm_ref manager=0;
 for(unsigned i=0;i<sizeof services/sizeof services[0];i++){
  a[0].l=dvm_new_string(v,services[i].name);
  NS_CHECK(services[i].name,ns_test_call(v,ctx,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)&&out.l&&dvm__obj(v,out.l)&&!strcmp(dvm__obj(v,out.l)->cls->name,services[i].type));
  fprintf(f,"pid=%d SERVICE name=%s type=%s PASS\n",getpid(),services[i].name,services[i].type);
  dvm_ref identity=out.l;NS_CHECK("service_cached_identity",ns_test_call(v,ctx,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)&&out.l==identity);
  if(i==0)manager=out.l;
 }
 a[0].l=dvm_new_string(v,"unsupported_sys_service_test");NS_CHECK("unknown_returns_null",ns_test_call(v,ctx,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)&&!out.l);
 /* These services have no host backend here. They remain unavailable. */
 const char *absent[]={"input_method","alarm","power","storage"};
 for(unsigned i=0;i<sizeof absent/sizeof absent[0];i++){a[0].l=dvm_new_string(v,absent[i]);NS_CHECK(absent[i],ns_test_call(v,ctx,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",a,1,&out)&&!out.l);fprintf(f,"pid=%d SERVICE name=%s type=null unavailable=1 PASS\n",getpid(),absent[i]);}
 struct dvm_class *channelclass=dvm_find_class_or_external(v,"android/app/NotificationChannel");
 dvm_ref ch=dvm_new_object(v,channelclass),id=dvm_new_string(v,"sys-service-selftest"),name=dvm_new_string(v,"test channel");
 a[0].l=id;a[1].l=name;a[2].i=3;
 NS_CHECK("channel_ctor",ch&&ns_test_call(v,ch,"<init>","(Ljava/lang/String;Ljava/lang/CharSequence;I)V",a,3,&out));
 NS_CHECK("channel_id",ns_test_call(v,ch,"getId","()Ljava/lang/String;",NULL,0,&out)&&out.l==id);
 NS_CHECK("channel_name",ns_test_call(v,ch,"getName","()Ljava/lang/CharSequence;",NULL,0,&out)&&out.l==name);
 NS_CHECK("channel_importance_3",ns_test_call(v,ch,"getImportance","()I",NULL,0,&out)&&out.i==3);
 NS_CHECK("channel_default_badge",ns_test_call(v,ch,"canShowBadge","()Z",NULL,0,&out)&&out.i==1);
 for(unsigned i=0;i<sizeof ns_properties/sizeof ns_properties[0];i++){
  const ns_property *p=&ns_properties[i];if(!p->set||!strcmp(p->field,"vibrationPattern"))continue;
  char sig[128];union dvm_value value={0};if(p->type[0]=='L')value.l=dvm_new_string(v,"stored property");else value.i=p->type[0]=='Z'?1:7;
  a[0]=value;snprintf(sig,sizeof sig,"(%s)V",p->type);NS_CHECK(p->set,ns_test_call(v,ch,p->set,sig,a,1,&out));
  snprintf(sig,sizeof sig,"()%s",p->type);NS_CHECK(p->get,ns_test_call(v,ch,p->get,sig,NULL,0,&out)&&(p->type[0]=='L'?out.l==value.l:out.i==value.i));
  if(p->type[0]=='Z'){a[0].i=0;snprintf(sig,sizeof sig,"(%s)V",p->type);NS_CHECK("boolean_clear",ns_test_call(v,ch,p->set,sig,a,1,&out));snprintf(sig,sizeof sig,"()%s",p->type);NS_CHECK("boolean_clear_read",ns_test_call(v,ch,p->get,sig,NULL,0,&out)&&out.i==0);}
 }
 dvm_ref pattern=dvm_new_array(v,'J',"J",2);((int64_t*)dvm_array_data(v,pattern))[0]=10;((int64_t*)dvm_array_data(v,pattern))[1]=20;
 a[0].l=pattern;NS_CHECK("pattern_set",ns_test_call(v,ch,"setVibrationPattern","([J)V",a,1,&out));
 ((int64_t*)dvm_array_data(v,pattern))[0]=99;
 NS_CHECK("pattern_defensive_copy",ns_test_call(v,ch,"getVibrationPattern","()[J",NULL,0,&out)&&out.l!=pattern&&((int64_t*)dvm_array_data(v,out.l))[0]==10);
 NS_CHECK("pattern_enables_vibration",ns_test_call(v,ch,"shouldVibrate","()Z",NULL,0,&out)&&out.i==1);
 dvm_ref uri=new_plain_object(v,"android/net/Uri","test sound"),attrs=new_plain_object(v,"android/media/AudioAttributes","test audio attributes");
 a[0].l=uri;a[1].l=attrs;NS_CHECK("setSound",ns_test_call(v,ch,"setSound","(Landroid/net/Uri;Landroid/media/AudioAttributes;)V",a,2,&out));
 NS_CHECK("getSound",ns_test_call(v,ch,"getSound","()Landroid/net/Uri;",NULL,0,&out)&&out.l==uri);
 NS_CHECK("getAudioAttributes",ns_test_call(v,ch,"getAudioAttributes","()Landroid/media/AudioAttributes;",NULL,0,&out)&&out.l==attrs);
 a[0].l=ch;NS_CHECK("manager_create",ns_test_call(v,manager,"createNotificationChannel","(Landroid/app/NotificationChannel;)V",a,1,&out));
 a[0].l=id;NS_CHECK("manager_get_same_identity",ns_test_call(v,manager,"getNotificationChannel","(Ljava/lang/String;)Landroid/app/NotificationChannel;",a,1,&out)&&out.l==ch);
 NS_CHECK("manager_enabled_true",ns_test_call(v,manager,"areNotificationsEnabled","()Z",NULL,0,&out)&&out.i==1);
 NS_CHECK("manager_list",ns_test_call(v,manager,"getNotificationChannels","()Ljava/util/List;",NULL,0,&out)&&out.l);
 dvm_ref list=out.l;NS_CHECK("manager_list_size_1",collections_call(v,"java/util/List","size","()I",list,NULL,0,&out)&&out.i==1);
 a[0].l=id;NS_CHECK("manager_delete",ns_test_call(v,manager,"deleteNotificationChannel","(Ljava/lang/String;)V",a,1,&out));
 NS_CHECK("manager_deleted_null",ns_test_call(v,manager,"getNotificationChannel","(Ljava/lang/String;)Landroid/app/NotificationChannel;",a,1,&out)&&!out.l);
 a[0].l=list;NS_CHECK("manager_create_channels",ns_test_call(v,manager,"createNotificationChannels","(Ljava/util/List;)V",a,1,&out));
 a[0].l=id;NS_CHECK("manager_bulk_restored",ns_test_call(v,manager,"getNotificationChannel","(Ljava/lang/String;)Landroid/app/NotificationChannel;",a,1,&out)&&out.l==ch);
 NS_CHECK("manager_test_channel_cleanup",ns_test_call(v,manager,"deleteNotificationChannel","(Ljava/lang/String;)V",a,1,&out));
 dvm_ref notification=new_plain_object(v,"android/app/Notification","test notification");a[0].i=47;a[1].l=notification;
 NS_CHECK("manager_notify_no_delivery",ns_test_call(v,manager,"notify","(ILandroid/app/Notification;)V",a,2,&out));
 NS_CHECK("manager_cancel",ns_test_call(v,manager,"cancel","(I)V",a,1,&out));
 NS_CHECK("manager_cancelAll",ns_test_call(v,manager,"cancelAll","()V",NULL,0,&out));
 ok=true;
done:
 fprintf(f,"pid=%d RESULT ok=%d checks=%u stage=%s moving_enabled=0 system_delivery=0\n",getpid(),ok,checks,stage);fclose(f);
#undef NS_CHECK
 return ok;
}
