from pathlib import Path
p=Path('login-milestone-next/strict_runtime_login.c')
s=p.read_text();s=s.replace('original-activity-v1 diagnostic_only','original-activity-lifecycle-v2 diagnostic_only')
s=s.replace('struct dvm_method *init=dvm_lookup(v,klass,"initialize","()V");','struct dvm_method *init=dvm_lookup(v,klass,"onCreate","(Landroid/os/Bundle;)V");')
s=s.replace('BEGIN original initialize; application attach and Android lifecycle have not run','BEGIN original onCreate with null Bundle; application attach has not run')
s=s.replace('ok=dvm_call(v,init,activity,NULL,0,&result);','union dvm_value bundle={.l=0};\n ok=dvm_call(v,init,activity,&bundle,1,&result);')
s=s.replace('END initialize returned=%d missing=%u','END onCreate returned=%d missing=%u')
p.write_text(s)
p=Path('login-milestone-next/activity_runtime.c');s=p.read_text();needle=' if(!self||self!=state.activity||state.lifecycle!=1)return false;';rep=''' if(self==state.activity && !strcmp(c,"android/app/Activity") && !strcmp(m,"onCreate") && !strcmp(sig,"(Landroid/os/Bundle;)V")){
  if(state.lifecycle!=1){dvm__throw(v,"java/lang/IllegalStateException","Activity.onCreate invalid state");return true;}
  state.lifecycle=2; /* CREATED; no window or UI service is installed. */
  return true;
 }
 if(!self||self!=state.activity||(state.lifecycle!=1&&state.lifecycle!=2))return false;''';assert needle in s;s=s.replace(needle,rep);p.write_text(s)
