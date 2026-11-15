from pathlib import Path
p=Path('login-milestone-next/strict_runtime_login.c')
s=p.read_text()
s='#include "activity_runtime.h"\n'+s
s=s.replace('if(executor_runtime_call(vm,','if(activity_runtime_call(vm,c,m,s,self,a,n,out)||executor_runtime_call(vm,',1)
s=s.replace('android_basics_vm_start(v);collections_vm_start(v);','android_basics_vm_start(v);activity_runtime_vm_start(v);collections_vm_start(v);',1)
pos=s.index('sdk_login_trace_run(')
s=s[:pos]+s[pos:].replace('dvm_set_trace(v,2);','dvm_set_trace(v,0);',1)
p.write_text(s)
