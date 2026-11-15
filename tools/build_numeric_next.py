"""Build the strict numeric parser candidate from the preserved real-thread-v9 snapshot."""
from pathlib import Path
import hashlib, json, shutil, subprocess

ROOT = Path(__file__).resolve().parent
BASE = ROOT / "build/thread-next/source"
OUT = ROOT / "build/numeric-next"
SRC = OUT / "source"
OUT.mkdir(parents=True, exist_ok=True)
shutil.copytree(BASE, SRC, dirs_exist_ok=True)
for name in ("numeric_runtime.c", "numeric_runtime.h", "numeric_runtime_test.c",
             "thread_runtime.c", "thread_runtime_test.c", "java_basics.c",
             "java_basics.h",
             "executor_runtime.c", "executor_runtime.h"):
    shutil.copy2(ROOT / name, SRC / name)

# The real-thread snapshot owns the Handler delegation and GIL changes in
# android_basics.c. Keep that candidate-specific plumbing, then layer the
# current SQLite helper/database contracts into the generated source.
android = SRC / "android_basics.c"
android_text = android.read_text(encoding="utf-8")
if "#include <ctype.h>" not in android_text:
    android_text = android_text.replace("#include <stdlib.h>", "#include <stdlib.h>\n#include <stdio.h>\n#include <ctype.h>", 1)
anchor = "struct handler_thread{dvm_ref name;int priority;};"
if "struct sqlite_helper" not in android_text:
    android_text = android_text.replace(anchor, anchor +
        "\nstruct sqlite_helper{dvm_ref context,name,factory,database;int version;};\n"
        "struct sqlite_database{dvm_ref name;bool open;unsigned exec_count;int64_t logical_count;};\n"
        "struct sqlite_cursor{int position,row_count;int64_t scalar;bool closed;};", 1)
# Keep the cursor contract identical to the hand-maintained source while the
# candidate continues to inherit its own thread/handler implementation.
if "sqlite_cursor_call" not in android_text:
    root_android = (ROOT / "android_basics.c").read_text(encoding="utf-8")
    helper_start = root_android.index("#define IS(name,signature)")
    helper_end = root_android.index("bool android_basics_call", helper_start)
    helper_block = root_android[helper_start:helper_end]
    marker = "#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))"
    if marker not in android_text:
        raise RuntimeError("android_basics.c: cursor helper marker missing")
    android_text = android_text.replace(marker, helper_block.rstrip(), 1)
sqlite_block = '''
 if(!strcmp(c,"android/database/sqlite/SQLiteOpenHelper")){
  if(IS("<init>","(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","SQLiteOpenHelper receiver");return true;}
   if(!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","SQLiteOpenHelper context/name");return true;}
   struct sqlite_helper *h=calloc(1,sizeof *h);if(!h){dvm__throw(v,"java/lang/OutOfMemoryError","SQLiteOpenHelper state");return true;}
   h->context=a[0].l;h->name=a[1].l;h->factory=a[2].l;h->version=a[3].i;free(o->data);o->data=h;return true;
  }
  struct dvm_object *o=dvm__obj(v,self);struct sqlite_helper *h=o?o->data:NULL;
  if(!h){dvm__throw(v,"java/lang/IllegalStateException","SQLiteOpenHelper not initialized");return true;}
  if(IS("getDatabaseName","()Ljava/lang/String;")){out->l=h->name;return true;}
  if(IS("setWriteAheadLoggingEnabled","(Z)V"))return true;
  if(IS("close","()V")){if(h->database){struct sqlite_database *db=dvm__obj(v,h->database)->data;if(db)db->open=false;}return true;}
  if(IS("getWritableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")||IS("getReadableDatabase","()Landroid/database/sqlite/SQLiteDatabase;")){
   if(!h->database){struct dvm_class *dbcls=dvm_find_class_or_external(v,"android/database/sqlite/SQLiteDatabase");h->database=dvm_new_object(v,dbcls);if(!h->database){dvm__throw(v,"java/lang/OutOfMemoryError","SQLiteDatabase");return true;}struct sqlite_database *db=calloc(1,sizeof *db);if(!db){dvm__throw(v,"java/lang/OutOfMemoryError","SQLiteDatabase state");return true;}db->name=h->name;db->open=true;dvm__obj(v,h->database)->data=db;
    struct dvm_method *create=dvm_find_method(v,dvm_object_class(v,self),"onCreate","(Landroid/database/sqlite/SQLiteDatabase;)V");if(create&&(create->has_code||create->builtin)){union dvm_value arg={.l=h->database},ignored={0};if(!dvm_call(v,create,self,&arg,1,&ignored))return true;}
   }else{struct sqlite_database *db=dvm__obj(v,h->database)->data;if(db&&!db->open){db->open=true;}}
   out->l=h->database;return true;
  }
  if(IS("onCreate","(Landroid/database/sqlite/SQLiteDatabase;)V")||IS("onUpgrade","(Landroid/database/sqlite/SQLiteDatabase;II)V")||IS("onDowngrade","(Landroid/database/sqlite/SQLiteDatabase;II)V")||IS("onOpen","(Landroid/database/sqlite/SQLiteDatabase;)V"))return true;
 }
 if(!strcmp(c,"android/database/sqlite/SQLiteDatabase")){
  struct dvm_object *o=dvm__obj(v,self);struct sqlite_database *db=o?o->data:NULL;
  if(IS("isOpen","()Z")){out->i=db&&db->open;return true;}
  if(IS("close","()V")){if(db)db->open=false;return true;}
  if(IS("getPath","()Ljava/lang/String;")){out->l=db?db->name:0;return true;}
  if(IS("execSQL","(Ljava/lang/String;)V")){
   if(!db||!db->open){dvm__throw(v,"java/lang/IllegalStateException","SQLiteDatabase is closed");return true;}
   if(!a[0].l||!dvm_string_utf8(v,a[0].l)){dvm__throw(v,"java/lang/NullPointerException","SQL statement");return true;}
   if(db->exec_count!=4294967295u)++db->exec_count;
   const char *sql=dvm_string_utf8(v,a[0].l);
   if(sqlite_contains_ci(sql,"insert")&&db->logical_count<9223372036854775807LL)++db->logical_count;
   else if(sqlite_contains_ci(sql,"delete")&&db->logical_count>0)--db->logical_count;
   return true;
  }
  if(IS("rawQuery","(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;")){
   if(!db||!db->open){dvm__throw(v,"java/lang/IllegalStateException","SQLiteDatabase is closed");return true;}
   if(!a[0].l||!dvm_string_utf8(v,a[0].l)){dvm__throw(v,"java/lang/NullPointerException","SQL query");return true;}
   const char *sql=dvm_string_utf8(v,a[0].l);struct dvm_class *cursor_cls=dvm_find_class_or_external(v,"android/database/Cursor");
   dvm_ref cursor=cursor_cls?dvm_new_object(v,cursor_cls):0;struct dvm_object *co=cursor?dvm__obj(v,cursor):NULL;struct sqlite_cursor *cur=co?calloc(1,sizeof *cur):NULL;
   if(!cursor||!co||!cur){free(cur);dvm__throw(v,"java/lang/OutOfMemoryError","SQLite cursor");return true;}
   cur->position=-1;
   if(sqlite_contains_ci(sql,"select")&&sqlite_contains_ci(sql,"count")){cur->row_count=1;cur->scalar=db->logical_count;}
   else cur->row_count=0;
   co->data=cur;out->l=cursor;return true;
  }
 }
 if(!strcmp(c,"android/database/Cursor"))return sqlite_cursor_call(v,m,sig,self,a,out);
'''
if "android/database/sqlite/SQLiteOpenHelper" not in android_text:
    marker = "\n return false;\n}"
    marker_at = android_text.rfind(marker)
    if marker_at < 0:
        raise RuntimeError("android_basics.c: SQLite insertion marker missing")
    android_text = android_text[:marker_at] + "\n" + sqlite_block + android_text[marker_at:]
android.write_text(android_text, encoding="utf-8")

def edit(name, old, new, count=1):
    path = SRC / name
    text = path.read_text(encoding="utf-8")
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f"{name}: expected {count} occurrences, found {actual}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")

for name in ("strict_runtime.c", "java_basics_test.c", "runtime_extra_test.inc",
             "thread_runtime_test.c"):
    path = SRC / name
    text = path.read_text(encoding="utf-8")
    if "real-thread-v9" not in text:
        raise RuntimeError(f"{name}: missing real-thread-v9 marker")
    path.write_text(text.replace("real-thread-v9", "numeric-next"), encoding="utf-8")
edit("strict_runtime.c", '#include "runtime_extra.h"', '#include "runtime_extra.h"\n#include "numeric_runtime.h"\n#include "executor_runtime.h"')
edit("strict_runtime.c", 'if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"<init>")&&!strcmp(s,"()V")) return true;', 'if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"<init>")&&!strcmp(s,"()V")) return true;\n /* Android Handler.handleMessage is an empty inherited base callback. */\n if(!strcmp(c,"android/os/Handler")&&!strcmp(m,"handleMessage")&&!strcmp(s,"(Landroid/os/Message;)V")) return true;')
edit("strict_runtime.c", "if(thread_runtime_call(vm,c,m,s,self,a,n,out)||runtime_extra_call(vm,", "if(executor_runtime_call(vm,c,m,s,self,a,n,out)||numeric_runtime_call(vm,c,m,s,self,a,n,out)||thread_runtime_call(vm,c,m,s,self,a,n,out)||runtime_extra_call(vm,")
edit("strict_runtime.c", " thread_runtime_shutdown(v,100);\n sdk_trace_fprintf(stderr,\"END_STAGE %s", " executor_runtime_shutdown(v,100);\n thread_runtime_shutdown(v,100);\n sdk_trace_fprintf(stderr,\"END_STAGE %s")

# Executor workers own their queue and are joined before the base thread
# runtime and before dvm_destroy tears down heap-backed task objects.
path = SRC / "generated/dvm/dvm.c"
text = path.read_text(encoding="utf-8")
if '#include "executor_runtime.h"' not in text:
    text = '#include "executor_runtime.h"\n' + text
text = text.replace('   thread_runtime_shutdown(vm,100);\n', '   executor_runtime_shutdown(vm,100);\n   thread_runtime_shutdown(vm,100);\n', 1)
path.write_text(text, encoding="utf-8")
edit("java_basics_test.c", '#include "runtime_extra.h"', '#include "runtime_extra.h"\n#include "numeric_runtime.h"')
edit("java_basics_test.c", "if(!runtime_extra_selftest(v,&checks))okay=false;if(!vm_arena_java_selftest(v))okay=false;if(!thread_runtime_selftest(v))okay=false;", "if(!runtime_extra_selftest(v,&checks))okay=false;if(!numeric_runtime_selftest(v))okay=false;if(!vm_arena_java_selftest(v))okay=false;if(!thread_runtime_selftest(v))okay=false;if(!executor_runtime_selftest(v,&checks))okay=false;")
edit("java_basics_test.c", '#include "runtime_extra.h"', '#include "runtime_extra.h"\n#include "executor_runtime.h"')

compiler = Path(r"C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/bin/clang.exe")
sdk = compiler.parents[2]
common = [str(compiler), "--target=aarch64-linux-ohos", "--sysroot=" + str(sdk / "sysroot"),
          "-std=gnu11", "-fPIC", "-O1", "-I" + str(SRC), "-I" + str(SRC / "generated")]
names = ["generated/dvm/dvm.c", "generated/dvm/dex.c", "strict_runtime.c",
         "java_basics.c", "java_basics_test.c", "android_basics.c",
         "android_basics_test.c", "guest_profile.c", "collections.c",
         "collections_test.c", "next_arena.c", "thread_runtime.c",
         "thread_runtime_test.c", "executor_runtime.c", "numeric_runtime.c", "numeric_runtime_test.c"]
objects = []
for name in names:
    obj = OUT / (Path(name).stem + ".o")
    args = common.copy()
    if name != "next_arena.c":
        args += ["-include", str(SRC / "next_interpose.h")]
    subprocess.run(args + ["-c", str(SRC / name), "-o", str(obj)], check=True)
    objects.append(str(obj))

nm = compiler.parent / "llvm-nm.exe"
allocation_symbols = {"malloc", "calloc", "realloc", "free", "strdup", "strndup", "mmap", "aligned_alloc", "posix_memalign"}
audit = []
for name, obj in zip(names, objects):
    if name == "next_arena.c":
        continue
    undefined = subprocess.check_output([str(nm), "--undefined-only", obj], text=True)
    bypass = allocation_symbols.intersection(line.split()[-1] for line in undefined.splitlines() if line.split())
    if bypass:
        raise RuntimeError(f"untracked allocation symbols in {name}: {sorted(bypass)}")
    audit.append({"translation_unit": name, "untracked_allocation_symbols": []})

library = OUT / "libsdktrace.so"
subprocess.run(common + ["-shared", *objects, "-Wl,--no-undefined", "-lm", "-o", str(library)], check=True)
runner = OUT / "sdktrace-runner"
subprocess.run(common + [str(SRC / "runner.c"), *objects, "-Wl,--no-undefined", "-lm", "-o", str(runner)], check=True)

manifest = {
    "candidate": "numeric-next",
    "base_candidate": "real-thread-v9",
    "base_sha256": hashlib.sha256((ROOT / "build/thread-next/libsdktrace.so").read_bytes()).hexdigest(),
    "candidate_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
    "bytes": library.stat().st_size,
    "execution_status": "compiled and linked; staged for user-operated device validation",
    "scope": "strict numeric parsing and isolated executor/queue worker lifecycle",
    "numeric_contract": {
        "signed_radix": "2..36",
        "overflow": "rejected with NumberFormatException",
        "trailing_input": "rejected",
        "whitespace": "rejected",
        "character_digit": "BMP Unicode decimal blocks and ASCII/fullwidth Latin digits",
        "boolean": "case-insensitive true only; all other values false",
    },
    "allocation_symbol_audit": audit,
    "limitations": [
        "No device HAP deployment or SDK stage rerun was performed.",
        "Numeric and executor selftests run only when the candidate is invoked by the HAP.",
        "Custom ThreadFactory and rejection handlers are accepted; worker creation remains runtime-owned. Interrupting a running FutureTask is unsupported.",
        "Original SDK initialization, login, network and Unity remain unverified.",
    ],
}
(OUT / "candidate.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
(OUT / "REVIEW.md").write_text(
    "# numeric-next diagnostic candidate\n\n"
    f"Artifact: `libsdktrace.so`, {library.stat().st_size} bytes.\n"
    f"SHA256: `{manifest['candidate_sha256']}`.\n\n"
    "The candidate is built from the preserved real-thread-v9 source snapshot. "
    "It adds strict numeric parsing plus bounded queue operations, independent "
    "executor workers, FutureTask completion and joined shutdown. Embedded "
    "semantic tests run in the HAP; prior artifacts remain unchanged. "
    "Compilation is not device validation.\n",
    encoding="utf-8",
)
print(json.dumps(manifest, indent=2))
