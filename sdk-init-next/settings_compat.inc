/* Explicit local compatibility policy for Secure/System. Does not change host
 * settings, grant permissions or synthesize device/location identifiers. */
extern void perf_diag_connectivity_detail(const char *,const char *) __attribute__((weak));
bool content_runtime_class_supported(const char *c){
 return c&&(!strcmp(c,"android/provider/Settings")||!strcmp(c,"android/provider/Settings$Secure")||
 !strcmp(c,"android/provider/Settings$System")||!strcmp(c,"android/content/ContentResolver")||
 !strcmp(c,"android/location/LocationManager")||!strcmp(c,"android/location/Location"));
}
static int compat_table(const char *c){return !strcmp(c,"android/provider/Settings$Secure")?2:!strcmp(c,"android/provider/Settings$System")?1:-1;}
bool content_runtime_method_supported(const char *c,const char *m,const char *s){
 if(!c||!m||!s)return false;
 if(!strcmp(c,"android/content/Context")&&!strcmp(m,"getContentResolver")&&!strcmp(s,"()Landroid/content/ContentResolver;"))return true;
 if(!strcmp(c,"android/location/LocationManager"))return (!strcmp(m,"getLastKnownLocation")&&!strcmp(s,"(Ljava/lang/String;)Landroid/location/Location;"))||(!strcmp(m,"isProviderEnabled")&&!strcmp(s,"(Ljava/lang/String;)Z"));
 if(compat_table(c)<0)return false;
 if(!strcmp(m,"getUriFor"))return !strcmp(s,"(Ljava/lang/String;)Landroid/net/Uri;");
 static const struct{const char *m,*s;} methods[]={
 {"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)I"},{"getInt","(Landroid/content/ContentResolver;Ljava/lang/String;)I"},
 {"getLong","(Landroid/content/ContentResolver;Ljava/lang/String;J)J"},{"getLong","(Landroid/content/ContentResolver;Ljava/lang/String;)J"},
 {"getFloat","(Landroid/content/ContentResolver;Ljava/lang/String;F)F"},{"getFloat","(Landroid/content/ContentResolver;Ljava/lang/String;)F"},
 {"getString","(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;"},
 {"getString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"},
 {"putInt","(Landroid/content/ContentResolver;Ljava/lang/String;I)Z"},
 {"putString","(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z"}};
 for(unsigned i=0;i<sizeof methods/sizeof methods[0];i++)if(!strcmp(m,methods[i].m)&&!strcmp(s,methods[i].s))return true;
 return false;
}
static void compat_note(int table,unsigned bit,const char *operation){
 if(compat_noted[table]&bit)return;compat_noted[table]|=bit;
 if(perf_diag_connectivity_detail){char b[200];snprintf(b,sizeof b,"class=%s op=%s hits=1 policy=local_stub_no_host_write",table==0?"LocationManager":table==1?"Settings$System":"Settings$Secure",operation);perf_diag_connectivity_detail("settings_stub",b);}
}
bool content_runtime_constant(struct dvm *v,const char *c,const char *field,const char *type,union dvm_value *out){
 if(!c||!field||!type||strcmp(type,"Ljava/lang/String;"))return false;
 const char *value=NULL;
 if(compat_table(c)>=0){
  static const struct{const char *f,*v;} fields[]={
   {"SCREEN_OFF_TIMEOUT","screen_off_timeout"},{"ANDROID_ID","android_id"},
   {"ACCELEROMETER_ROTATION","accelerometer_rotation"},{"USER_ROTATION","user_rotation"},
   {"LOCATION_MODE","location_mode"},{"LOCATION_PROVIDERS_ALLOWED","location_providers_allowed"},
   {"SCREEN_BRIGHTNESS","screen_brightness"},{"FONT_SCALE","font_scale"}};
  for(unsigned i=0;i<sizeof fields/sizeof fields[0];i++)if(!strcmp(field,fields[i].f)){value=fields[i].v;break;}
 }else if(!strcmp(c,"android/location/LocationManager")){
  if(!strcmp(field,"GPS_PROVIDER"))value="gps";
  if(!strcmp(field,"NETWORK_PROVIDER"))value="network";
  if(!strcmp(field,"PASSIVE_PROVIDER"))value="passive";
 }
 if(!value)return false;if(!v||!out)return true;
 out->l=dvm_new_string(v,value);if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Settings constant");return true;
}
static bool compat_settings_call(struct dvm *v,const char *c,const char *m,const char *sig,
 const union dvm_value *a,int n,union dvm_value *out){
 int table=compat_table(c);if(table<0||!content_runtime_method_supported(c,m,sig))return false;
 if(!strcmp(m,"getUriFor"))return false; /* Existing real Uri implementation. */
 bool no_default=!strcmp(sig,"(Landroid/content/ContentResolver;Ljava/lang/String;)I")||!strcmp(sig,"(Landroid/content/ContentResolver;Ljava/lang/String;)J")||!strcmp(sig,"(Landroid/content/ContentResolver;Ljava/lang/String;)F")||!strcmp(sig,"(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
 if(!a||n!=(no_default?2:3))return false;
 if(!resolver_arg(v,a[0].l))return true;
 const char *key=key_arg(v,a[1].l);if(!key)return true;
 if(!strncmp(m,"put",3)){
  if(n!=3)return false;
  /* 64 entries per namespace, bounded key/value lengths, VM lifetime only. */
  unsigned count=0;for(settings_entry *e=settings[table];e;e=e->next)count++;
  const char *text;char number[32];
  if(!strcmp(m,"putInt")){snprintf(number,sizeof number,"%d",a[2].i);text=number;}
  else text=a[2].l?dvm_string_utf8(v,a[2].l):NULL;
  if(a[2].l&&!strcmp(m,"putString")&&!text){dvm__throw(v,"java/lang/IllegalArgumentException","Settings value is not a String");return true;}
  if(strlen(key)>256||(text&&strlen(text)>4096)||(!entry_find(table,key)&&count>=64)){out->i=0;return true;}
  out->i=entry_set(v,table,key,text);compat_note(table,1,"put_memory");return true;
 }
 const char *text=entry_value(table,key);char host[CONTENT_SETTINGS_HOST_VALUE_MAX];
 /* Keep the existing verified System host mappings. Unknown Android keys
  * have no host mapping and use the requested local default policy. */
 bool mapped=table==SETTINGS_SYSTEM&&(!strcmp(key,"screen_off_timeout")||!strcmp(key,"screen_brightness")||!strcmp(key,"font_scale")||!strcmp(key,"accelerometer_rotation")||!strcmp(key,"user_rotation"));
 if(!text&&mapped&&host_api&&host_api->get){int rc=host_api->get(host_user,table,key,host,sizeof host);host[sizeof host-1]=0;if(rc==1)text=host;else if(rc<0)return setting_host_failure(v,rc,table,key,"get");}
 compat_note(table,2,"get_default_or_memory");
 if(!strcmp(m,"getInt")){int32_t x;out->i=integer_text(text,&x)?x:n==3?a[2].i:0;}
 else if(!strcmp(m,"getLong")){int64_t x;out->j=long_text(text,&x)?x:n==3?a[2].j:0;}
 else if(!strcmp(m,"getFloat")){float x;out->f=float_text(text,&x)?x:n==3?a[2].f:0;}
 else if(!strcmp(m,"getString")){if(!text&&n==3){out->l=a[2].l;return true;}out->l=dvm_new_string(v,text?text:"");if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Settings string");}
 return true;
}
