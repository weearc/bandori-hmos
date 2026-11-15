#include "bridge.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>
struct cls { void *identity; };
struct binding { void *identity; std::string name, sig; void *fn; };
struct jb_vm;
struct env { const void *const *table; jb_vm *owner; std::string exception; };
struct jb_vm { const void *const *table; jb_provider provider; std::mutex lock; std::vector<cls *> classes; std::vector<binding> methods; std::atomic<int> attached{0}; };
static thread_local env *current;
static const void *env_table[233] = {}, *vm_table[8] = {};
static std::once_flag setup;
static void fail(env *e,const char *s) { if(e && e->exception.empty()) e->exception=s; }
static bool valid(env *e) { return e && current==e; }
static bool descriptor(const char *s) {
 if (!s || *s++!='(') return false;
 auto type=[](const char *&p,bool allow_void)->bool { while (*p=='[') { ++p; allow_void=false; } if (*p=='L') { ++p; const char *b=p; while(*p && *p!=';') ++p; if(p==b || *p!=';') return false; ++p; return true; } if(*p && strchr("ZBCSIJFD",*p)) {++p; return true;} if(allow_void && *p=='V') {++p;return true;} return false; };
 while(*s && *s!=')') if(!type(s,false)) return false;
 if(*s++!=')' || !type(s,true)) return false; return *s==0;
}
static int attach(jb_vm *v, void **out, void *) {
 if(!v || !out) return -1; *out=nullptr;
 if(current) { if(current->owner!=v) return -1; *out=current; return 0; }
 env *e=new(std::nothrow) env{env_table,v,{}}; if(!e) return -4; current=e; ++v->attached; *out=e; return 0;
}
static int detach(jb_vm *v) { if(!current || current->owner!=v) return -1; delete current; current=nullptr; --v->attached; return 0; }
static int getenv_jni(jb_vm *v,void **out,int version) { if(!out)return -1; *out=nullptr; if(version!=0x10006 && version!=0x10004 && version!=0x10002 && version!=0x10001)return -3; if(!current || current->owner!=v)return -2; *out=current;return 0; }
static void *find_class(env *e,const char *name) {
 if(!valid(e) || !name || !*name) { fail(e,"invalid class request");return nullptr; } if(!e->exception.empty())return nullptr;
 void *id=e->owner->provider.find_class(e->owner->provider.context,name); if(!id) {fail(e,"class not found");return nullptr;}
 std::lock_guard<std::mutex> g(e->owner->lock); for(auto c:e->owner->classes) if(c->identity==id)return c;
 cls *c=new(std::nothrow) cls{id}; if(!c){fail(e,"out of memory");return nullptr;} try {e->owner->classes.push_back(c);}catch(...){delete c;fail(e,"out of memory");return nullptr;} return c;
}
static int register_natives(env *e,void *ref,const jb_native *methods,int n) {
 if(!valid(e) || !ref || n<0 || (n && !methods)) {fail(e,"invalid registration");return -1;} if(!e->exception.empty()) return -1;
 jb_vm *v=e->owner; std::lock_guard<std::mutex> g(v->lock); cls *c=nullptr; for(auto p:v->classes)if(p==ref)c=p; if(!c){fail(e,"invalid class reference");return -1;}
 try { auto next=v->methods; for(int i=0;i<n;++i) {auto &m=methods[i]; if(!m.name || !*m.name || !descriptor(m.signature) || !m.fn || !v->provider.is_native_method(v->provider.context,c->identity,m.name,m.signature)){fail(e,"native method not found or invalid");return -1;} bool found=false; for(auto &b:next)if(b.identity==c->identity && b.name==m.name && b.sig==m.signature){b.fn=m.fn;found=true;break;} if(!found)next.push_back({c->identity,m.name,m.signature,m.fn});} v->methods.swap(next); return 0;} catch(...){fail(e,"out of memory");return -1;}
}
static void fatal(env *,const char *) { std::abort(); }
static int get_vm(env *e,void **out){if(!valid(e)||!out)return -1;*out=e->owner;return 0;}
static void exception_clear(env *e){if(valid(e))e->exception.clear();}
static unsigned char exception_check(env *e){return valid(e)&&!e->exception.empty();}
extern "C" jb_vm *jb_create(const jb_provider *p) { if(!p||!p->find_class||!p->is_native_method)return nullptr; std::call_once(setup,[]{vm_table[4]=(void*)attach;vm_table[5]=(void*)detach;vm_table[6]=(void*)getenv_jni;vm_table[7]=(void*)attach; env_table[6]=(void*)find_class;env_table[17]=(void*)exception_clear;env_table[18]=(void*)fatal;env_table[215]=(void*)register_natives;env_table[219]=(void*)get_vm;env_table[228]=(void*)exception_check;}); auto v=new(std::nothrow) jb_vm; if(v){v->table=vm_table;v->provider=*p;}return v; }
extern "C" int jb_destroy(jb_vm *v){if(!v||v->attached.load())return -1;for(auto c:v->classes)delete c;delete v;return 0;}
extern "C" void *jb_java_vm(jb_vm *v){return v;}
extern "C" void *jb_lookup_native(jb_vm *v,void *id,const char *n,const char *s){if(!v||!id||!n||!s)return nullptr;std::lock_guard<std::mutex> g(v->lock);for(auto &b:v->methods)if(b.identity==id&&b.name==n&&b.sig==s)return b.fn;return nullptr;}
extern "C" const char *jb_pending_exception(jb_vm *v){return current&&current->owner==v&&!current->exception.empty()?current->exception.c_str():nullptr;}
extern "C" void jb_clear_exception(jb_vm *v){if(current&&current->owner==v)current->exception.clear();}
