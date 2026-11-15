/*
 * 自有 JNI 桥纯逻辑 selftest。
 *
 * 本程序不启动 DVM、不加载任何动态库、不访问设备。它只覆盖与 DVM 解耦的
 * 部分：ABI 槽位、token、句柄表、local frame、mangling、签名解析、jvalue
 * 边界、JNIEnv/JavaVM 的 C ABI 调用约定，以及未实现槽的 typed stub 表。
 *
 * 需要真实 DVM / 设备 / libjni_probe.so 的用例一律输出 NOT_RUN，并让最终
 * RESULT 成为 INCOMPLETE；绝不把未执行的路径打印成 PASS。
 *
 * 退出码：
 *   0 = 全部 PASS
 *   1 = 至少一个 FAIL
 *   2 = 无 FAIL，但存在 NOT_RUN（INCOMPLETE）
 *
 * 若要把本文件与 jni_bridge.c 链接（设备侧测试），请定义
 * JNI_SELFTEST_NO_STUB_TABLE，避免与 jni_bridge.c 中的
 * jni_abi_stub_fail 定义冲突；stub_table 用例会转为 NOT_RUN。
 */

#include "jni_abi.h"
#include "jni_handles.h"
#include "jni_util.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(JNI_SELFTEST_NO_STUB_TABLE)
#include "jni_abi_stubs.h"
#endif

static unsigned g_total;
static unsigned g_pass;
static unsigned g_fail;
static unsigned g_not_run;
static char g_detail[768];

static void reset_detail(void)
{
    g_detail[0] = '\0';
}

static bool expect(bool cond, const char *fmt, ...)
{
    if (cond) return true;
    if (g_detail[0] == '\0') {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_detail, sizeof(g_detail), fmt, ap);
        va_end(ap);
    }
    return false;
}

static void report(const char *name, bool ok, const char *pass_detail)
{
    ++g_total;
    if (ok) {
        ++g_pass;
        printf("JNI_BRIDGE_SELFTEST %s PASS %s\n", name, pass_detail);
    } else {
        ++g_fail;
        printf("JNI_BRIDGE_SELFTEST %s FAIL %s\n", name, g_detail);
    }
}

static void report_not_run(const char *name, const char *detail)
{
    ++g_total;
    ++g_not_run;
    printf("JNI_BRIDGE_SELFTEST %s NOT_RUN %s\n", name, detail);
}

/* ------------------------------------------------------------------ */
/* 句柄表 fake pin/unpin                                              */
/* ------------------------------------------------------------------ */

static unsigned g_pin_calls;
static unsigned g_unpin_calls;

static bool fake_pin(void *ctx, uint32_t object_ref)
{
    (void)ctx;
    if (object_ref == 0) return false;
    ++g_pin_calls;
    return true;
}

static void fake_unpin(void *ctx, uint32_t object_ref)
{
    (void)ctx;
    (void)object_ref;
    ++g_unpin_calls;
}

static const jni_handle_callbacks g_fake_callbacks = {
    NULL,
    fake_pin,
    fake_unpin,
};

/* ------------------------------------------------------------------ */
/* 1. ABI 槽位                                                        */
/* ------------------------------------------------------------------ */

static void test_abi_slots(void)
{
    reset_detail();
    bool ok = true;

    int check = jni_abi_check();
    ok &= expect(check == -1, "jni_abi_check 返回 %d，期望 -1", check);
    ok &= expect(sizeof(struct JNINativeInterface_) == 233 * sizeof(void *),
                 "JNINativeInterface 大小 %zu，期望 %zu",
                 sizeof(struct JNINativeInterface_), 233 * sizeof(void *));
    ok &= expect(sizeof(struct JNIInvokeInterface_) == 8 * sizeof(void *),
                 "JNIInvokeInterface 大小 %zu，期望 %zu",
                 sizeof(struct JNIInvokeInterface_), 8 * sizeof(void *));
    ok &= expect(JNI_VERSION_1_6 == 0x00010006, "JNI_VERSION_1_6 数值错误");
    ok &= expect(JNI_COMMIT == 1 && JNI_ABORT == 2, "JNI_COMMIT/JNI_ABORT 错误");
    ok &= expect(JNIInvalidRefType == 0 && JNILocalRefType == 1 &&
                     JNIGlobalRefType == 2 && JNIWeakGlobalRefType == 3,
                 "jobjectRefType 枚举值错误");

    report("abi_slots", ok, "233/8 槽、关键 offsetof 与常量校验通过");
}

/* ------------------------------------------------------------------ */
/* 2. token 编解码                                                    */
/* ------------------------------------------------------------------ */

static void test_token(void)
{
    reset_detail();
    bool ok = true;
    jni_token_parts p;
    jobject t = jni_token_null();

    ok &= expect(jni_token_is_null(t), "token 0 未被识别为 null");
    ok &= expect(!jni_token_decode(t, &p), "token 0 不应解码成功");

    t = jni_token_null();
    ok &= expect(!jni_token_encode(UINT32_MAX, 0, 1, &t),
                 "index == UINT32_MAX 应被拒绝");
    ok &= expect(jni_token_is_null(t), "编码失败后应输出 null");

    t = jni_token_null();
    ok &= expect(jni_token_encode(0, 0, 1, &t), "encode(0,0,1) 失败");
    ok &= expect(!jni_token_is_null(t), "encode 结果不应为 null");
    ok &= expect(jni_token_decode(t, &p), "decode 失败");
    ok &= expect(p.index == 0 && p.generation == 0 && p.vm_tag == 1,
                 "decode 结果 index=%u gen=%u vm=%u",
                 p.index, p.generation, (unsigned)p.vm_tag);

    t = jni_token_null();
    ok &= expect(jni_token_encode(0x12345678u, 0x00abcdefu, 0x5au, &t),
                 "encode 大值失败");
    ok &= expect(jni_token_decode(t, &p), "decode 大值失败");
    ok &= expect(p.index == 0x12345678u && p.generation == 0x00abcdefu &&
                     p.vm_tag == 0x5au,
                 "大值 decode 结果 index=%u gen=%u vm=%u",
                 p.index, p.generation, (unsigned)p.vm_tag);
    ok &= expect((jni_token_raw(t) >> 32) == 0x5aabcdefu,
                 "token 位布局错误：raw=0x%016llx",
                 (unsigned long long)jni_token_raw(t));

    report("token", ok, "编码/解码/0/null/溢出边界");
}

/* ------------------------------------------------------------------ */
/* 3. 句柄表基础语义                                                  */
/* ------------------------------------------------------------------ */

static void test_handles_basic(void)
{
    reset_detail();
    bool ok = true;
    jni_handle_table table;
    uint32_t ref = 0;
    jni_ref_kind kind = JNI_REF_NONE;

    g_pin_calls = 0;
    g_unpin_calls = 0;

    ok &= expect(jni_handles_init(&table, 16, 7, &g_fake_callbacks),
                 "jni_handles_init 失败");
    if (!ok) {
        report("handles_basic", false, "");
        return;
    }

    jobject l = jni_handle_new_local(&table, 42);
    ok &= expect(!jni_token_is_null(l), "new_local 失败");
    ok &= expect(jni_handle_resolve_object(&table, l, JNI_REF_LOCAL, &ref,
                                           &kind),
                 "resolve local 失败");
    ok &= expect(ref == 42 && kind == JNI_REF_LOCAL,
                 "local 解析为 ref=%u kind=%d", ref, (int)kind);
    ok &= expect(jni_handle_ref_type(&table, l) == JNILocalRefType,
                 "local ref_type 错误");

    jobject l2 = jni_handle_new_local_ref(&table, l);
    ok &= expect(!jni_token_is_null(l2), "NewLocalRef 失败");
    ok &= expect(jni_handle_resolve_object(&table, l2, JNI_REF_LOCAL, &ref,
                                           &kind) && ref == 42,
                 "NewLocalRef 解析失败");

    jobject g2 = jni_handle_new_global_ref(&table, l);
    ok &= expect(!jni_token_is_null(g2), "NewGlobalRef 失败");
    ok &= expect(jni_handle_resolve_object(&table, g2, JNI_REF_GLOBAL, &ref,
                                           &kind) && ref == 42,
                 "NewGlobalRef 解析失败");
    ok &= expect(jni_handle_ref_type(&table, g2) == JNIGlobalRefType,
                 "global ref_type 错误");

    jobject g = jni_handle_new_global(&table, 42);
    jobject w = jni_handle_new_weak(&table, 42);
    ok &= expect(!jni_token_is_null(g), "new_global 失败");
    ok &= expect(!jni_token_is_null(w), "new_weak 失败");
    ok &= expect(jni_handle_resolve_object(&table, w, JNI_REF_WEAK, &ref,
                                           &kind) && ref == 42,
                 "weak 解析失败");
    ok &= expect(jni_handle_ref_type(&table, w) == JNIWeakGlobalRefType,
                 "weak ref_type 错误");

    ok &= expect(jni_handle_is_same(&table, l, g) == JNI_TRUE,
                 "IsSameObject(local, global) 应为 true");
    ok &= expect(jni_handle_live_count(&table) == 5,
                 "live_count=%u，期望 5", jni_handle_live_count(&table));
    ok &= expect(jni_handle_pinned_count(&table) == 4,
                 "pinned_count=%u，期望 4", jni_handle_pinned_count(&table));
    ok &= expect(g_pin_calls == 4, "pin 次数=%u，期望 4", g_pin_calls);

    ok &= expect(jni_handle_delete_local_any(&table, l2), "删除 l2 失败");
    ok &= expect(jni_handle_delete_global(&table, g2), "删除 g2 失败");
    ok &= expect(!jni_handle_delete_global(&table, g2), "重复删除 g2 应失败");

    ok &= expect(jni_handle_delete_local_any(&table, l), "删除 l 失败");
    ok &= expect(!jni_handle_delete_local_any(&table, l),
                 "重复删除 l 应失败");
    ok &= expect(!jni_handle_resolve_object(&table, l, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "已删除 token 仍可解析");

    ok &= expect(jni_handle_delete_global(&table, g), "删除 g 失败");
    ok &= expect(jni_handle_delete_weak(&table, w), "删除 w 失败");
    ok &= expect(jni_handle_pinned_count(&table) == 0,
                 "删除后 pinned_count=%u", jni_handle_pinned_count(&table));
    ok &= expect(g_pin_calls == g_unpin_calls,
                 "pin/unpin 不平衡：%u/%u", g_pin_calls, g_unpin_calls);

    ok &= expect(jni_token_is_null(jni_handle_new_local(&table, 0)),
                 "new_local(0) 应返回 null");
    ok &= expect(jni_token_is_null(jni_handle_new_global(&table, 0)),
                 "new_global(0) 应返回 null");
    ok &= expect(!jni_handle_resolve_object(&table, NULL, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "null token 不应解析成功");

    jni_handles_shutdown(&table);
    ok &= expect(jni_handle_live_count(&table) == 0, "shutdown 后 live 非 0");
    ok &= expect(jni_handle_pinned_count(&table) == 0,
                 "shutdown 后 pinned 非 0");
    ok &= expect(g_pin_calls == g_unpin_calls,
                 "shutdown 后 pin/unpin 不平衡：%u/%u", g_pin_calls,
                 g_unpin_calls);

    report("handles_basic", ok,
           "local/global/weak、NewLocalRef/NewGlobalRef、pin/unpin、删除");
}

/* ------------------------------------------------------------------ */
/* 4. generation / vm_tag / index 伪造拒绝                            */
/* ------------------------------------------------------------------ */

static void test_handle_generation(void)
{
    reset_detail();
    bool ok = true;
    jni_handle_table a;
    jni_handle_table b;
    uint32_t ref = 0;
    jni_ref_kind kind = JNI_REF_NONE;
    jni_token_parts pa;
    jni_token_parts pb;

    g_pin_calls = 0;
    g_unpin_calls = 0;

    ok &= expect(jni_handles_init(&a, 1, 3, &g_fake_callbacks),
                 "表 a init 失败");
    if (!ok) {
        report("handle_generation", false, "");
        return;
    }
    ok &= expect(jni_handles_init(&b, 1, 4, &g_fake_callbacks),
                 "表 b init 失败");

    jobject t1 = jni_handle_new_local(&a, 11);
    ok &= expect(!jni_token_is_null(t1), "分配 t1 失败");
    ok &= expect(jni_handle_delete_local_any(&a, t1), "删除 t1 失败");
    jobject t2 = jni_handle_new_local(&a, 22);
    ok &= expect(!jni_token_is_null(t2), "分配 t2 失败");

    ok &= expect(jni_token_decode(t1, &pa), "decode t1 失败");
    ok &= expect(jni_token_decode(t2, &pb), "decode t2 失败");
    ok &= expect(pa.index == pb.index, "容量 1 的表应复用同一槽位");
    ok &= expect(pa.generation != pb.generation,
                 "复用槽位后 generation 未变化（%u）", pa.generation);
    ok &= expect(!jni_handle_resolve_object(&a, t1, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "旧 generation 的 token 仍可解析");
    ok &= expect(jni_handle_resolve_object(&a, t2, JNI_REF_LOCAL, &ref,
                                           &kind) && ref == 22,
                 "新 token 解析失败或 ref 错误");

    jobject forged = jni_token_null();
    ok &= expect(jni_token_encode(pb.index, (pb.generation + 1u) & 0x00ffffffu,
                                  3, &forged),
                 "构造伪造 generation 失败");
    ok &= expect(!jni_handle_resolve_object(&a, forged, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "伪造 generation 未被拒绝");

    forged = jni_token_null();
    ok &= expect(jni_token_encode(pb.index, pb.generation, 4, &forged),
                 "构造跨 VM token 失败");
    ok &= expect(!jni_handle_resolve_object(&a, forged, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "跨 VM token 未被拒绝");
    ok &= expect(!jni_handle_resolve_object(&b, t2, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "表 b 不应接受表 a 的 token");

    forged = jni_token_null();
    ok &= expect(jni_token_encode(5, 0, 3, &forged),
                 "构造越界 index token 失败");
    ok &= expect(!jni_handle_resolve_object(&a, forged, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "越界 index token 未被拒绝");

    jni_handles_shutdown(&b);
    jni_handles_shutdown(&a);
    ok &= expect(g_pin_calls == g_unpin_calls,
                 "pin/unpin 不平衡：%u/%u", g_pin_calls, g_unpin_calls);

    report("handle_generation", ok,
           "generation 复用、伪造 index/generation/vm_tag 拒绝");
}

/* ------------------------------------------------------------------ */
/* 5. local frame                                                     */
/* ------------------------------------------------------------------ */

static void test_handle_frames(void)
{
    reset_detail();
    bool ok = true;
    jni_handle_table table;
    uint32_t ref = 0;
    jni_ref_kind kind = JNI_REF_NONE;

    g_pin_calls = 0;
    g_unpin_calls = 0;

    ok &= expect(jni_handles_init(&table, 16, 9, &g_fake_callbacks),
                 "init 失败");
    if (!ok) {
        report("handle_frames", false, "");
        return;
    }

    uint32_t f1 = jni_handle_current_frame(&table);
    ok &= expect(f1 != 0, "初始 frame 为 0");

    jobject outer = jni_handle_new_local(&table, 100);
    ok &= expect(!jni_token_is_null(outer), "分配 outer 失败");

    uint32_t f2 = jni_handle_frame_push(&table);
    ok &= expect(f2 != 0 && f2 != f1, "push frame 失败");
    jobject inner = jni_handle_new_local(&table, 200);
    ok &= expect(!jni_token_is_null(inner), "分配 inner 失败");
    ok &= expect(!jni_handle_delete_local(&table, outer),
                 "外层 local 不应能在内层 frame 删除");
    ok &= expect(jni_handle_delete_local(&table, inner), "删除当前 local 失败");

    inner = jni_handle_new_local(&table, 201);
    ok &= expect(!jni_token_is_null(inner), "重新分配 inner 失败");
    ok &= expect(jni_handle_frame_pop(&table, f2), "pop frame 失败");
    ok &= expect(!jni_handle_resolve_object(&table, inner, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "pop 后 inner 仍可解析");
    ok &= expect(jni_handle_resolve_object(&table, outer, JNI_REF_LOCAL, &ref,
                                           &kind) && ref == 100,
                 "pop 后 outer 不应失效");
    ok &= expect(!jni_handle_frame_pop(&table, f1),
                 "基础 frame 不应允许 pop");

    uint32_t f3 = jni_handle_frame_push(&table);
    ok &= expect(f3 != 0, "push f3 失败");
    jobject r = jni_handle_new_local(&table, 300);
    jobject promoted = jni_token_null();
    ok &= expect(jni_handle_frame_pop_with_result(&table, f3, r, &promoted),
                 "PopLocalFrame(result) 失败");
    ok &= expect(!jni_token_is_null(promoted), "提升后的 token 为 null");
    ok &= expect(!jni_handle_resolve_object(&table, r, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "被 pop 的 result 仍可解析");
    ok &= expect(jni_handle_resolve_object(&table, promoted, JNI_REF_LOCAL,
                                           &ref, &kind) && ref == 300,
                 "提升后的 token 解析失败");
    ok &= expect(jni_handle_ref_type(&table, promoted) == JNILocalRefType,
                 "提升后的 token 不是 local");
    ok &= expect(jni_handle_current_frame(&table) == f1,
                 "pop 后当前 frame 未回到 f1");

    uint32_t f4 = jni_handle_frame_push(&table);
    uint32_t f5 = jni_handle_frame_push(&table);
    ok &= expect(f4 != 0 && f5 != 0, "push f4/f5 失败");
    ok &= expect(!jni_handle_frame_pop(&table, f4),
                 "非 LIFO pop 应失败");
    ok &= expect(jni_handle_frame_pop(&table, f5), "pop f5 失败");
    ok &= expect(jni_handle_frame_pop(&table, f4), "pop f4 失败");

    uint32_t f6 = jni_handle_frame_push(&table);
    jobject out2 = (jobject)(uintptr_t)1;
    ok &= expect(jni_handle_frame_pop_with_result(&table, f6, NULL, &out2),
                 "PopLocalFrame(NULL) 失败");
    ok &= expect(jni_token_is_null(out2), "NULL result 应输出 null");

    jni_handles_shutdown(&table);
    ok &= expect(g_pin_calls == g_unpin_calls,
                 "pin/unpin 不平衡：%u/%u", g_pin_calls, g_unpin_calls);

    report("handle_frames", ok,
           "push/pop、跨帧删除拒绝、PopLocalFrame(result)、LIFO");
}

/* ------------------------------------------------------------------ */
/* 6. class token                                                     */
/* ------------------------------------------------------------------ */

static void test_handle_class(void)
{
    reset_detail();
    bool ok = true;
    static int dummy_class;
    jni_handle_table table;
    void *cls = NULL;
    uint32_t ref = 0;
    jni_ref_kind kind = JNI_REF_NONE;

    g_pin_calls = 0;
    g_unpin_calls = 0;

    ok &= expect(jni_handles_init(&table, 8, 5, &g_fake_callbacks),
                 "init 失败");
    if (!ok) {
        report("handle_class", false, "");
        return;
    }

    jobject c = jni_handle_new_class(&table, &dummy_class);
    ok &= expect(!jni_token_is_null(c), "new_class 失败");
    ok &= expect(jni_handle_resolve_class(&table, c, &cls) &&
                     cls == (void *)&dummy_class,
                 "class token 解析失败");
    ok &= expect(!jni_handle_resolve_object(&table, c, JNI_REF_LOCAL, &ref,
                                            &kind),
                 "class token 不应解析为对象");
    ok &= expect(jni_handle_ref_type(&table, c) == JNILocalRefType,
                 "class token 生命周期类型不是 local");

    jobject l = jni_handle_new_local_ref(&table, c);
    jobject g = jni_handle_new_global_ref(&table, c);
    jobject g2 = jni_handle_new_global_ref(&table, g);
    ok &= expect(!jni_token_is_null(l) && !jni_token_is_null(g) &&
                     !jni_token_is_null(g2),
                 "class ref 复制失败");
    ok &= expect(jni_handle_resolve_class(&table, l, &cls) &&
                     cls == (void *)&dummy_class,
                 "local class ref 解析失败");
    ok &= expect(jni_handle_resolve_class(&table, g, &cls) &&
                     cls == (void *)&dummy_class,
                 "global class ref 解析失败");
    ok &= expect(jni_handle_resolve_class(&table, g2, &cls) &&
                     cls == (void *)&dummy_class,
                 "global class ref 二次解析失败");
    ok &= expect(jni_handle_ref_type(&table, g) == JNIGlobalRefType,
                 "global class ref 类型错误");
    ok &= expect(jni_handle_pinned_count(&table) == 0,
                 "class token 不应 pin，实际 pinned=%u",
                 jni_handle_pinned_count(&table));

    ok &= expect(jni_handle_delete_local_any(&table, c), "删除 c 失败");
    ok &= expect(jni_handle_delete_local_any(&table, l), "删除 l 失败");
    ok &= expect(jni_handle_delete_global(&table, g), "删除 g 失败");
    ok &= expect(jni_handle_delete_global(&table, g2), "删除 g2 失败");

    jni_handles_shutdown(&table);
    ok &= expect(g_pin_calls == 0 && g_unpin_calls == 0,
                 "class token 触发了 pin/unpin：%u/%u", g_pin_calls,
                 g_unpin_calls);

    report("handle_class", ok, "class token 解析、local/global 生命周期");
}

/* ------------------------------------------------------------------ */
/* 7. mangling                                                        */
/* ------------------------------------------------------------------ */

static void test_mangling(void)
{
    reset_detail();
    bool ok = true;
    char out[256];

    ok &= expect(jni_mangle_ident("com/hmos/probe/JniProbe", out, sizeof(out)),
                 "mangle 类名失败");
    ok &= expect(strcmp(out, "com_hmos_probe_JniProbe") == 0,
                 "类名 mangling 得到 %s", out);

    ok &= expect(jni_java_symbol_short("com/hmos/probe/JniProbe", "staticAdd",
                                       out, sizeof(out)),
                 "短名生成失败");
    ok &= expect(strcmp(out,
                        "Java_com_hmos_probe_JniProbe_staticAdd") == 0,
                 "短名得到 %s", out);

    ok &= expect(jni_java_symbol_long("com/hmos/probe/JniProbe", "staticAdd",
                                      "(II)I", out, sizeof(out)),
                 "长名 (II)I 生成失败");
    ok &= expect(strcmp(out,
                        "Java_com_hmos_probe_JniProbe_staticAdd__II") == 0,
                 "长名 (II)I 得到 %s", out);

    ok &= expect(jni_java_symbol_long(
                     "com/hmos/probe/JniProbe", "concat",
                     "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                     out, sizeof(out)),
                 "长名对象参数生成失败");
    ok &= expect(strcmp(
                     out,
                     "Java_com_hmos_probe_JniProbe_concat__Ljava_lang_String_2"
                     "Ljava_lang_String_2") == 0,
                 "长名对象参数得到 %s", out);

    ok &= expect(jni_java_symbol_long("com/hmos/probe/JniProbe", "take",
                                      "([I)V", out, sizeof(out)),
                 "数组参数长名生成失败");
    ok &= expect(strcmp(out,
                        "Java_com_hmos_probe_JniProbe_take___3I") == 0,
                 "数组参数长名得到 %s", out);

    ok &= expect(jni_mangle_ident("com/hmos/a_b/C", out, sizeof(out)),
                 "下划线类名 mangling 失败");
    ok &= expect(strcmp(out, "com_hmos_a_1b_C") == 0,
                 "下划线类名得到 %s", out);

    ok &= expect(jni_mangle_ident("a;b[c", out, sizeof(out)),
                 "特殊字符 mangling 失败");
    ok &= expect(strcmp(out, "a_2b_3c") == 0,
                 "特殊字符得到 %s", out);

    ok &= expect(jni_mangle_ident("a-b", out, sizeof(out)),
                 "其他字符 mangling 失败");
    ok &= expect(strcmp(out, "a_0002db") == 0,
                 "其他字符得到 %s", out);

    char small[6];
    memset(small, 0x7f, sizeof(small));
    ok &= expect(!jni_mangle_ident("abcdef", small, sizeof(small)),
                 "缓冲不足时不应报告成功");
    ok &= expect(small[0] == '\0', "mangling 失败后未清空输出");

    out[0] = 'x';
    ok &= expect(!jni_java_symbol_long("com/hmos/probe/JniProbe", "bad",
                                       "(I", out, sizeof(out)),
                 "非法签名不应生成长名");
    ok &= expect(out[0] == '\0', "长名失败后未清空输出");

    out[0] = 'x';
    ok &= expect(!jni_mangle_sig_args("(V)V", out, sizeof(out)),
                 "参数中的 V 应被拒绝");
    ok &= expect(out[0] == '\0', "参数 mangling 失败后未清空输出");

    report("mangling", ok, "短名/长名、_1/_2/_3/_0xxxx、缓冲与非法签名");
}

/* ------------------------------------------------------------------ */
/* 8. 签名解析                                                        */
/* ------------------------------------------------------------------ */

static void test_signature(void)
{
    reset_detail();
    bool ok = true;
    jni_sig_info info;

    ok &= expect(jni_sig_parse("(ILjava/lang/String;[J)D", &info),
                 "解析 (ILjava/lang/String;[J)D 失败");
    ok &= expect(info.nargs == 3, "参数个数 %d，期望 3", info.nargs);
    ok &= expect(info.args[0].kind == JNI_KIND_INT &&
                     info.args[0].desc_len == 1 &&
                     info.args[0].desc[0] == 'I',
                 "参数 0 不是 I");
    ok &= expect(info.args[1].kind == JNI_KIND_OBJECT &&
                     info.args[1].desc_len == 18 &&
                     memcmp(info.args[1].desc, "Ljava/lang/String;", 18) == 0,
                 "参数 1 不是 Ljava/lang/String;");
    ok &= expect(info.args[2].kind == JNI_KIND_ARRAY &&
                     info.args[2].desc_len == 2 &&
                     memcmp(info.args[2].desc, "[J", 2) == 0,
                 "参数 2 不是 [J");
    ok &= expect(info.ret == JNI_KIND_DOUBLE && info.ret_desc_len == 1 &&
                     info.ret_desc[0] == 'D',
                 "返回类型不是 D");

    ok &= expect(jni_sig_parse("()V", &info), "解析 ()V 失败");
    ok &= expect(info.nargs == 0 && info.ret == JNI_KIND_VOID,
                 "()V 结果错误");

    ok &= expect(jni_sig_parse("([Ljava/lang/String;)V", &info),
                 "解析对象数组失败");
    ok &= expect(info.nargs == 1 && info.args[0].kind == JNI_KIND_ARRAY &&
                     info.args[0].desc_len == 19,
                 "对象数组参数描述错误");

    ok &= expect(!jni_sig_parse("(V)V", &info), "(V)V 应被拒绝");
    ok &= expect(!jni_sig_parse("(I", &info), "未闭合签名应被拒绝");
    ok &= expect(!jni_sig_parse("(I)", &info), "缺少返回类型应被拒绝");
    ok &= expect(!jni_sig_parse("(I)Vx", &info), "尾随字符应被拒绝");
    ok &= expect(!jni_sig_parse("I)V", &info), "缺少左括号应被拒绝");
    ok &= expect(!jni_sig_parse("(L;)V", &info), "空类名应被拒绝");
    ok &= expect(!jni_sig_parse("([Q)V", &info), "非法数组元素应被拒绝");
    ok &= expect(!jni_sig_parse("", &info), "空签名应被拒绝");

    ok &= expect(jni_kind_width(JNI_KIND_BOOLEAN) == 1 &&
                     jni_kind_width(JNI_KIND_CHAR) == 2 &&
                     jni_kind_width(JNI_KIND_INT) == 4 &&
                     jni_kind_width(JNI_KIND_LONG) == 8 &&
                     jni_kind_width(JNI_KIND_DOUBLE) == 8 &&
                     jni_kind_width(JNI_KIND_OBJECT) == sizeof(void *),
                 "jni_kind_width 错误");
    ok &= expect(strcmp(jni_kind_name(JNI_KIND_ARRAY), "array") == 0,
                 "jni_kind_name 错误");

    report("signature", ok, "参数/返回类型解析、非法签名拒绝");
}

/* ------------------------------------------------------------------ */
/* 9. jvalue 边界                                                     */
/* ------------------------------------------------------------------ */

static void test_jvalue(void)
{
    reset_detail();
    bool ok = true;
    jvalue v;

    ok &= expect(sizeof(jvalue) == 8, "sizeof(jvalue)=%zu，期望 8",
                 sizeof(jvalue));
    ok &= expect(offsetof(jvalue, z) == 0 && offsetof(jvalue, j) == 0 &&
                     offsetof(jvalue, d) == 0 && offsetof(jvalue, l) == 0,
                 "jvalue 字段偏移非 0");
    ok &= expect(sizeof(jboolean) == 1 && sizeof(jbyte) == 1 &&
                     sizeof(jchar) == 2 && sizeof(jshort) == 2 &&
                     sizeof(jint) == 4 && sizeof(jlong) == 8 &&
                     sizeof(jfloat) == 4 && sizeof(jdouble) == 8,
                 "JNI 基本类型宽度错误");
    ok &= expect(sizeof(jobject) == sizeof(void *),
                 "jobject 不是指针宽度");

    memset(&v, 0, sizeof(v));
    v.j = INT64_C(0x1122334455667788);
    uint64_t jbits = UINT64_C(0x1122334455667788);
    ok &= expect(v.j == INT64_C(0x1122334455667788),
                 "jlong 被截断：0x%016llx", (unsigned long long)v.j);
    ok &= expect(memcmp(&v.j, &jbits, sizeof(jbits)) == 0,
                 "jlong 的 64 位表示不一致");

    memset(&v, 0, sizeof(v));
    v.j = -1;
    jbits = UINT64_MAX;
    ok &= expect(memcmp(&v.j, &jbits, sizeof(jbits)) == 0,
                 "负 jlong 的 64 位表示错误");

    uint64_t dbits = UINT64_C(0x3fd5555555555555);
    memset(&v, 0, sizeof(v));
    memcpy(&v.d, &dbits, sizeof(dbits));
    ok &= expect(memcmp(&v.d, &dbits, sizeof(dbits)) == 0,
                 "jdouble 位模式被改变");

    memset(&v, 0, sizeof(v));
    v.l = (jobject)(uintptr_t)UINT64_C(0x123456789abcdef0);
    ok &= expect((uint64_t)(uintptr_t)v.l == UINT64_C(0x123456789abcdef0),
                 "jobject 被截断");

    memset(&v, 0, sizeof(v));
    v.i = -123456789;
    ok &= expect(v.i == -123456789, "jint 有符号值错误");

    report("jvalue", ok, "8 字节联合、long/double/对象不截断");
}

/* ------------------------------------------------------------------ */
/* 10. JNIEnv C ABI                                                   */
/* ------------------------------------------------------------------ */

struct fake_env {
    const struct JNINativeInterface_ *functions;
    uint32_t marker;
};

static struct JNINativeInterface_ g_fake_env_table;

static jint JNICALL fake_GetVersion(JNIEnv *env)
{
    if (!env) return JNI_ERR;
    if (*env != &g_fake_env_table) return JNI_ERR;
    return JNI_VERSION_1_6;
}

static jint fake_native_entry(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (*env)->GetVersion(env);
}

static void test_env_abi(void)
{
    reset_detail();
    bool ok = true;

    memset(&g_fake_env_table, 0, sizeof(g_fake_env_table));
    g_fake_env_table.GetVersion = fake_GetVersion;

    struct fake_env fe;
    memset(&fe, 0, sizeof(fe));
    fe.functions = &g_fake_env_table;
    fe.marker = 0x5a5a;

    JNIEnv *env = (JNIEnv *)(void *)&fe;
    ok &= expect(offsetof(struct fake_env, functions) == 0,
                 "fake_env 首字段偏移非 0");
    ok &= expect((*env)->GetVersion(env) == JNI_VERSION_1_6,
                 "(*env)->GetVersion 调用失败");
    ok &= expect(fake_native_entry(env, NULL) == JNI_VERSION_1_6,
                 "native 入口风格的 env 调用失败");
    ok &= expect(*(const struct JNINativeInterface_ * const *)(void *)&fe ==
                     &g_fake_env_table,
                 "JNIEnv 首字不是函数表指针");

    report("env_abi", ok, "JNIEnv 首字段函数表、native 入口调用约定");
}

/* ------------------------------------------------------------------ */
/* 11. JavaVM C ABI                                                   */
/* ------------------------------------------------------------------ */

static struct JNIInvokeInterface_ g_fake_vm_table;

static jint JNICALL fake_GetEnv(JavaVM *vm, void **penv, jint version)
{
    if (!vm || !penv) return JNI_EINVAL;
    if (*vm != (JavaVM)(void *)&g_fake_vm_table) return JNI_EINVAL;
    if (version != JNI_VERSION_1_6) return JNI_EVERSION;
    static int env_marker;
    *penv = &env_marker;
    return JNI_OK;
}

struct fake_vm_handle {
    const struct JNIInvokeInterface_ *functions;
    uint32_t marker;
};

static void test_vm_abi(void)
{
    reset_detail();
    bool ok = true;

    memset(&g_fake_vm_table, 0, sizeof(g_fake_vm_table));
    g_fake_vm_table.GetEnv = fake_GetEnv;

    JavaVM vm_value = (JavaVM)(void *)&g_fake_vm_table;
    JavaVM *vm = &vm_value;
    void *penv = NULL;

    ok &= expect((*vm)->GetEnv(vm, &penv, JNI_VERSION_1_6) == JNI_OK,
                 "C 风格 GetEnv 失败");
    ok &= expect(penv != NULL, "GetEnv 未写出 penv");
    ok &= expect((*vm)->GetEnv(vm, &penv, JNI_VERSION_1_4) == JNI_EVERSION,
                 "版本不匹配未返回 JNI_EVERSION");

    struct fake_vm_handle fvh;
    memset(&fvh, 0, sizeof(fvh));
    fvh.functions = &g_fake_vm_table;
    fvh.marker = 0x1234;
    JavaVM *vm2 = (JavaVM *)(void *)&fvh;
    ok &= expect(offsetof(struct fake_vm_handle, functions) == 0,
                 "fake_vm_handle 首字段偏移非 0");
    ok &= expect((*vm2)->GetEnv(vm2, &penv, JNI_VERSION_1_6) == JNI_OK,
                 "C++ 风格首字段 GetEnv 失败");
    ok &= expect(offsetof(struct JNIInvokeInterface_, AttachCurrentThread) ==
                     4 * sizeof(void *),
                 "AttachCurrentThread 槽位错误");
    ok &= expect(offsetof(struct JNIInvokeInterface_, GetEnv) ==
                     6 * sizeof(void *),
                 "GetEnv 槽位错误");
    ok &= expect(g_fake_vm_table.reserved0 == NULL &&
                     g_fake_vm_table.reserved1 == NULL &&
                     g_fake_vm_table.reserved2 == NULL,
                 "JNIInvokeInterface reserved0..2 非 NULL");

    report("vm_abi", ok, "JavaVM 值/*vm 约定、C/C++ 首字段兼容");
}

/* ------------------------------------------------------------------ */
/* 12. 未实现槽 typed stub 表                                         */
/* ------------------------------------------------------------------ */

#if !defined(JNI_SELFTEST_NO_STUB_TABLE)

static int g_stub_fail_count;
static int g_stub_last_slot;
static const char *g_stub_last_name;

int jni_abi_stub_fail(JNIEnv *env, int slot, const char *name)
{
    (void)env;
    ++g_stub_fail_count;
    g_stub_last_slot = slot;
    g_stub_last_name = name;
    return 0;
}

static void test_stub_table(void)
{
    reset_detail();
    bool ok = true;
    static struct JNINativeInterface_ table;

    memset(&table, 0, sizeof(table));
    jni_abi_stubs_install(&table);

    ok &= expect(table.reserved0 == NULL && table.reserved1 == NULL &&
                     table.reserved2 == NULL && table.reserved3 == NULL,
                 "reserved0..3 应保持 NULL");
    ok &= expect(table.GetVersion != NULL, "GetVersion 未安装 stub");
    ok &= expect(table.FindClass != NULL, "FindClass 未安装 stub");
    ok &= expect(table.GetObjectRefType != NULL, "GetObjectRefType 未安装 stub");

    unsigned nulls = 0;
    for (int i = 4; i < 233; ++i) {
        void *p = NULL;
        memcpy(&p, (unsigned char *)&table + (size_t)i * sizeof(void *),
               sizeof(p));
        if (!p) ++nulls;
    }
    ok &= expect(nulls == 0, "非 reserved 槽中有 %u 个 NULL", nulls);

    struct fake_env fe;
    memset(&fe, 0, sizeof(fe));
    fe.functions = &table;
    JNIEnv *env = (JNIEnv *)(void *)&fe;

    g_stub_fail_count = 0;
    g_stub_last_slot = -1;
    g_stub_last_name = NULL;
    jint v = (*env)->GetVersion(env);
    ok &= expect(v == 0, "GetVersion stub 返回 %d，期望 0", v);
    ok &= expect(g_stub_fail_count == 1 && g_stub_last_slot == 4 &&
                     g_stub_last_name != NULL &&
                     strcmp(g_stub_last_name, "GetVersion") == 0,
                 "GetVersion stub 未记录槽 4");

    jclass c = (*env)->FindClass(env, "x");
    ok &= expect(c == NULL && g_stub_last_slot == 6,
                 "FindClass stub 未返回 NULL 或槽号错误");

    jfloat f = (*env)->GetFloatField(env, NULL, NULL);
    ok &= expect(f == 0.0f && g_stub_last_slot == 102,
                 "GetFloatField stub 返回 %f，槽 %d", (double)f,
                 g_stub_last_slot);

    jdouble d = (*env)->GetDoubleField(env, NULL, NULL);
    ok &= expect(d == 0.0 && g_stub_last_slot == 103,
                 "GetDoubleField stub 返回 %f，槽 %d", d, g_stub_last_slot);

    jobjectRefType rt = (*env)->GetObjectRefType(env, NULL);
    ok &= expect(rt == JNIInvalidRefType && g_stub_last_slot == 232,
                 "GetObjectRefType stub 返回 %d，槽 %d", (int)rt,
                 g_stub_last_slot);

    report("stub_table", ok,
           "229 个非保留槽均非 NULL，int/pointer/float/double 返回 0");
}

#else

static void test_stub_table(void)
{
    report_not_run("stub_table",
                   "编译时定义 JNI_SELFTEST_NO_STUB_TABLE（与 jni_bridge.c 链接）");
}

#endif

/* ------------------------------------------------------------------ */
/* 13. 需要真实 DVM / 设备 / libjni_probe.so 的用例                    */
/* ------------------------------------------------------------------ */

static void report_dvm_dependent_not_run(void)
{
    report_not_run("bridge_lifecycle",
                   "需要真实 struct dvm；jni_bridge_init/install_hooks/shutdown 未执行");
    report_not_run("dvm_find_class",
                   "需要真实 DVM 类表与 FindClass 解析");
    report_not_run("string_roundtrip",
                   "需要真实 DVM 字符串与 modified UTF-8 往返");
    report_not_run("call_native_probe",
                   "需要加载 libjni_probe.so 并进入 jni_bridge_call_native");
    report_not_run("throw_pending",
                   "需要真实 ThrowNew 与 take_pending_exception 交接");
    report_not_run("register_natives",
                   "需要真实 DEX native 方法校验与 RegisterNatives 回滚");
    report_not_run("jni_onload_version",
                   "需要真实 dlopen/JNI_OnLoad 版本协商");
    report_not_run("native_local_frame",
                   "需要真实 native 调用边界的 local frame 自动释放");
    report_not_run("stub_pending_exception",
                   "需要真实 jni_abi_stub_fail 设置 UnsatisfiedLinkError");
    report_not_run("array_access",
                   "需要真实 DVM 数组对象与元素类");
    report_not_run("method_field_ids",
                   "需要真实 dvm_method/dvm_field 与 ID 表校验");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_abi_slots();
    test_token();
    test_handles_basic();
    test_handle_generation();
    test_handle_frames();
    test_handle_class();
    test_mangling();
    test_signature();
    test_jvalue();
    test_env_abi();
    test_vm_abi();
    test_stub_table();
    report_dvm_dependent_not_run();

    printf("JNI_BRIDGE_SELFTEST SUMMARY total=%u pass=%u fail=%u not_run=%u\n",
           g_total, g_pass, g_fail, g_not_run);

    const char *result = "PASS";
    if (g_fail > 0) result = "FAIL";
    else if (g_not_run > 0) result = "INCOMPLETE";
    printf("JNI_BRIDGE_SELFTEST RESULT %s\n", result);

    if (g_fail > 0) return 1;
    if (g_not_run > 0) return 2;
    return 0;
}
