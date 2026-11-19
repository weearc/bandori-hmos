#include <sys/stat.h>
#include <unistd.h>
/* Included once by jni_bridge.c: exercises the installed JNI function table,
 * real guest objects, retained copies, weak identities and direct loans. */
struct jni_gc_test_root {dvm_ref ref;unsigned hits;};
static void jni_gc_test_visit(uint32_t *slot,void *user)
{
    struct jni_gc_test_root *s=user;if(slot&&*slot==s->ref)++s->hits;
}

static bool jni_gc_test_pending_is(struct jni_thread_env *t,const char *name)
{
    struct dvm_object *o=dvm__obj(t->bridge->vm,t->pending);
    return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);
}

bool jni_gc_selftest(struct dvm *vm,const char *path)
{
    if(!vm||!path)return false;
    struct jni_bridge *b=jni_bridge_for_vm(vm);if(!b)return false;
    bool owned=!dvm_gil_held(vm);if(owned)dvm_gil_acquire(vm);
    struct jni_thread_env *t=thread_ensure(b);
    if(!t||t->pending||dvm_exception(vm)){if(owned)dvm_gil_release(vm);return false;}
    FILE *f=fopen(path,"w");if(!f){if(owned)dvm_gil_release(vm);return false;}
    if(fchmod(fileno(f),0644)){fclose(f);if(owned)dvm_gil_release(vm);return false;}
    JNIEnv *env=(JNIEnv *)(void *)t;
    unsigned checks=0;bool ok=false,frame=false;const char *stage="begin";
    dvm_ref direct_ref=0,copy_ref=0,string_ref=0,weak_ref=0,reused_ref=0,other_ref=0;
    jarray direct=NULL,copied=NULL,other=NULL,weak_local=NULL,reused=NULL;
    jstring string=NULL,string_alias=NULL;jweak weak=NULL;jobject global=NULL;
#define JNI_GC_CHECK(label,expression) do{stage=label;if(!(expression))goto done;++checks;fprintf(f,"PASS %s\n",label);fflush(f);}while(0)
    JNI_GC_CHECK("jni_root_providers_registered",b->gc_strong_registered&&b->gc_weak_registered);
    JNI_GC_CHECK("push_actual_jni_local_frame",(*env)->PushLocalFrame(env,32)==JNI_OK);frame=true;
    direct=(*env)->NewByteArray(env,4);other=(*env)->NewByteArray(env,4);
    JNI_GC_CHECK("real_jni_primitive_arrays",resolve_object_any(b,direct,&direct_ref)&&direct_ref&&resolve_object_any(b,other,&other_ref)&&other_ref);
    struct dvm_object *o=dvm__obj(vm,direct_ref);unsigned pins=o->pins;
    jboolean is_copy=JNI_TRUE;
    jbyte *p=(*env)->GetPrimitiveArrayCritical(env,direct,&is_copy);
    jbyte *p2=(*env)->GetPrimitiveArrayCritical(env,direct,NULL);
    JNI_GC_CHECK("critical_direct_pointer_not_copy",p&&p==o->data&&p2==p&&is_copy==JNI_FALSE);
    JNI_GC_CHECK("nested_payload_borrows_each_have_checked_pin",o->gc_payload_borrows==2&&o->gc_body_borrows==0&&o->gc_strong_handles==2&&o->pins==pins+2);
    global=(*env)->NewGlobalRef(env,direct);
    struct jni_gc_test_root roots={.ref=direct_ref};
    JNI_GC_CHECK("jni_local_and_global_strong_roots",global&&jni_gc_strong_roots(vm,jni_gc_test_visit,&roots,b)&&roots.hits==2);
    (*env)->DeleteGlobalRef(env,global);global=NULL;
    (*env)->DeleteLocalRef(env,direct);
    JNI_GC_CHECK("deleted_local_keeps_critical_identity",dvm__obj(vm,direct_ref)==o&&o->pins==2&&o->gc_payload_borrows==2);
    roots.hits=0;
    JNI_GC_CHECK("critical_handles_remain_authoritative_strong_roots",dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,jni_gc_test_visit,&roots)&&roots.hits==2);
    dvm__discard_temporary(vm,direct_ref);
    JNI_GC_CHECK("borrowed_temporary_cannot_be_reclaimed",dvm__obj(vm,direct_ref)==o);
    (*env)->ReleasePrimitiveArrayCritical(env,direct,p,77);
    JNI_GC_CHECK("critical_invalid_mode_rejected_without_releasing",jni_gc_test_pending_is(t,"java/lang/IllegalArgumentException")&&o->gc_payload_borrows==2);
    (*env)->ExceptionClear(env);
    (*env)->ReleasePrimitiveArrayCritical(env,direct,p+1,0);
    JNI_GC_CHECK("critical_invalid_pointer_rejected_without_releasing",jni_gc_test_pending_is(t,"java/lang/IllegalArgumentException")&&o->gc_payload_borrows==2);
    (*env)->ExceptionClear(env);
    (*env)->ReleasePrimitiveArrayCritical(env,other,p,0);
    JNI_GC_CHECK("critical_wrong_array_rejected_without_releasing",jni_gc_test_pending_is(t,"java/lang/IllegalArgumentException")&&o->gc_payload_borrows==2);
    (*env)->ExceptionClear(env);
    p[0]=41;(*env)->ReleasePrimitiveArrayCritical(env,direct,p,JNI_COMMIT);
    JNI_GC_CHECK("critical_commit_keeps_acquisition",o->pins==2&&o->gc_payload_borrows==2&&((jbyte *)o->data)[0]==41);
    (*env)->ReleasePrimitiveArrayCritical(env,direct,p,0);
    JNI_GC_CHECK("critical_final_release_drops_exactly_one_nested_loan",o->pins==1&&o->gc_payload_borrows==1&&o->gc_strong_handles==1);
    (*env)->ReleasePrimitiveArrayCritical(env,direct,p2,JNI_ABORT);
    JNI_GC_CHECK("critical_abort_drops_final_loan_without_undoing_direct_write",!o->pins&&!o->gc_payload_borrows&&!o->gc_strong_handles&&((jbyte *)o->data)[0]==41);
    (*env)->ReleasePrimitiveArrayCritical(env,direct,p2,0);
    JNI_GC_CHECK("critical_duplicate_final_release_rejected",jni_gc_test_pending_is(t,"java/lang/IllegalArgumentException")&&!o->gc_payload_borrows);
    (*env)->ExceptionClear(env);

    copied=(*env)->NewByteArray(env,4);
    JNI_GC_CHECK("copy_array_identity",resolve_object_any(b,copied,&copy_ref)&&copy_ref);
    struct dvm_object *co=dvm__obj(vm,copy_ref);
    jbyte *copy=(*env)->GetByteArrayElements(env,copied,&is_copy);
    JNI_GC_CHECK("elements_copy_is_root_not_pointer_pin",copy&&copy!=co->data&&is_copy==JNI_TRUE&&co->gc_strong_handles==1&&!co->gc_payload_borrows&&!co->gc_body_borrows);
    (*env)->DeleteLocalRef(env,copied);copy[0]=12;
    (*env)->ReleaseByteArrayElements(env,copied,copy,77);
    JNI_GC_CHECK("elements_invalid_mode_preserves_copy",jni_gc_test_pending_is(t,"java/lang/IllegalArgumentException")&&co->gc_strong_handles==1&&((jbyte *)co->data)[0]==0);
    (*env)->ExceptionClear(env);
    (*env)->ReleaseByteArrayElements(env,copied,copy,JNI_COMMIT);
    JNI_GC_CHECK("elements_commit_after_deleted_local_writes_real_array",!t->pending&&((jbyte *)co->data)[0]==12&&co->gc_strong_handles==1);
    copy[0]=29;(*env)->ReleaseByteArrayElements(env,copied,copy,JNI_ABORT);
    JNI_GC_CHECK("elements_abort_releases_retention_without_copyback",!t->pending&&!co->gc_strong_handles&&((jbyte *)co->data)[0]==12);

    string=(*env)->NewStringUTF(env,"gc2-identity");
    JNI_GC_CHECK("real_jni_string",resolve_object_any(b,string,&string_ref)&&string_ref);
    struct dvm_object *so=dvm__obj(vm,string_ref);
    const jchar *u16=(*env)->GetStringChars(env,string,&is_copy);
    string_alias=(*env)->NewLocalRef(env,string);
    const jchar *u16again=(*env)->GetStringCritical(env,string_alias,NULL);
    const char *utf=(*env)->GetStringUTFChars(env,string,NULL);
    JNI_GC_CHECK("string_copies_share_stable_identity_retention",u16&&u16again==u16&&utf&&!strcmp(utf,"gc2-identity")&&is_copy==JNI_TRUE&&so->gc_strong_handles==1&&!so->gc_payload_borrows&&!so->gc_body_borrows);
    (*env)->DeleteLocalRef(env,string);
    (*env)->DeleteLocalRef(env,string_alias);
    dvm__discard_temporary(vm,string_ref);
    JNI_GC_CHECK("deleted_string_local_cannot_reclaim_outstanding_copies",dvm__obj(vm,string_ref)==so&&u16[0]=='g'&&utf[0]=='g');
    (*env)->ReleaseStringChars(env,string,u16);
    JNI_GC_CHECK("nested_utf16_release_preserves_other_loans",so->gc_strong_handles==1&&u16again[0]=='g');
    (*env)->ReleaseStringUTFChars(env,string,utf);
    JNI_GC_CHECK("utf8_release_keeps_utf16_identity",so->gc_strong_handles==1&&u16again[0]=='g');
    (*env)->ReleaseStringCritical(env,string_alias,u16again);
    JNI_GC_CHECK("last_string_copy_release_drops_retention",!t->pending&&!so->gc_strong_handles);

    weak_local=(*env)->NewByteArray(env,1);
    JNI_GC_CHECK("weak_target_identity",resolve_object_any(b,weak_local,&weak_ref)&&weak_ref);
    uint64_t old_identity=dvm__obj(vm,weak_ref)->identity_generation;
    weak=(*env)->NewWeakGlobalRef(env,weak_local);(*env)->DeleteLocalRef(env,weak_local);
    roots=(struct jni_gc_test_root){.ref=weak_ref};
    JNI_GC_CHECK("weak_excluded_from_strong_jni_provider",weak&&jni_gc_strong_roots(vm,jni_gc_test_visit,&roots,b)&&!roots.hits);
    JNI_GC_CHECK("weak_enumerated_in_separate_provider",jni_gc_weak_roots(vm,jni_gc_test_visit,&roots,b)&&roots.hits==1);
    dvm__discard_temporary(vm,weak_ref);
    JNI_GC_CHECK("weak_does_not_retain_guest",!dvm__obj(vm,weak_ref));
    reused=(*env)->NewByteArray(env,1);
    JNI_GC_CHECK("real_guest_id_reused_for_weak_test",resolve_object_any(b,reused,&reused_ref)&&reused_ref==weak_ref&&dvm__obj(vm,reused_ref)->identity_generation!=old_identity);
    dvm_ref stale=123;
    JNI_GC_CHECK("weak_old_identity_clears_instead_of_aliasing_reused_id",resolve_object_any(b,weak,&stale)&&!stale&&(*env)->IsSameObject(env,weak,NULL)==JNI_TRUE);
    jobject cleared_local=(*env)->NewLocalRef(env,weak);
    jobject cleared_global=(*env)->NewGlobalRef(env,weak);
    JNI_GC_CHECK("promoting_cleared_weak_returns_null_without_exception",!cleared_local&&!cleared_global&&!t->pending);
    (*env)->DeleteWeakGlobalRef(env,weak);weak=NULL;

    jclass error_class=(*env)->FindClass(env,"java/lang/IllegalStateException");
    JNI_GC_CHECK("real_jni_pending_exception",error_class&&(*env)->ThrowNew(env,error_class,"gc2 pending root")==JNI_OK&&t->pending);
    roots=(struct jni_gc_test_root){.ref=t->pending};
    JNI_GC_CHECK("pending_exception_is_thread_root",jni_gc_strong_roots(vm,jni_gc_test_visit,&roots,b)&&roots.hits==1);
    (*env)->ExceptionClear(env);
    JNI_GC_CHECK("no_pending_or_vm_exception_after_actual_jni_checks",!t->pending&&!dvm_exception(vm));
    ok=true;
done:
    /* Only test-owned caches/loans are removed; pre-existing native loans are
     * not touched if this check runs on an already initialized live VM. */
    for(struct jni_critical_borrow **it=&t->critical;*it;){
        struct jni_critical_borrow *c=*it;
        if(c->ref==direct_ref){*it=c->next;(void)dvm_gc_unpin_checked(vm,c->ref);dvm_gc_handle_drop(vm,c->retention);free(c);}else it=&c->next;
    }
    for(int i=0;i<JNI_MAX_ARRAY_CACHE;i++)if(t->arrays[i].used&&t->arrays[i].ref==copy_ref)clear_array_cache(&t->arrays[i]);
    for(struct jni_utf_cache *c=t->strings;c;c=c->next)if(c->used&&c->ref==string_ref)clear_utf_cache(c);
    if(global)(*env)->DeleteGlobalRef(env,global);
    if(weak)(*env)->DeleteWeakGlobalRef(env,weak);
    if(t->pending)(*env)->ExceptionClear(env);
    if(dvm_exception(vm)){ok=false;dvm_clear_exception(vm);}
    if(frame)(*env)->PopLocalFrame(env,NULL);
    dvm_ref cleanup[]={direct_ref,copy_ref,string_ref,reused_ref,other_ref};
    for(unsigned i=0;i<sizeof cleanup/sizeof cleanup[0];i++)if(cleanup[i])dvm__discard_temporary(vm,cleanup[i]);
    if(weak_ref&&weak_ref!=reused_ref)dvm__discard_temporary(vm,weak_ref);
    fprintf(f,"JNI_GC_SELFTEST %s pid=%d checks=%u failed_stage=%s direct_payload=1 copy_retention=1 string_retention=1 weak_identity=1 collection_enabled=0 moving_enabled=0\n",ok?"PASS":"FAIL",getpid(),checks,ok?"none":stage);
    if(ferror(f))ok=false;if(fclose(f))ok=false;
    if(owned)dvm_gil_release(vm);
#undef JNI_GC_CHECK
    return ok;
}
