/*
 * jobject token 句柄表实现。
 *
 * 重要所有权规则：
 *   - local/global 对象引用在创建时 pin，删除/弹帧时 unpin；
 *   - weak 不 pin；
 *   - class 引用保存 void *cls，不 pin，按 local/global 生命周期管理；
 *   - shutdown 必须清空所有 pin，否则 DVM 堆无法释放。
 */

#include "jni_handles.h"

#include <stdlib.h>
#include <string.h>

static void pop_frame_slots(jni_handle_table *table, uint32_t frame_id);

static uint32_t table_current_frame(const jni_handle_table *table)
{
    if (!table || table->frame_depth == 0) return 0;
    return table->frame_stack[table->frame_depth - 1];
}

static jni_handle_slot *slot_from_token(jni_handle_table *table, jobject token,
                                        jni_token_parts *parts)
{
    if (!table || !parts) return NULL;
    if (!jni_token_decode(token, parts)) return NULL;
    if (parts->vm_tag != table->vm_tag) return NULL;
    if (parts->index >= table->capacity) return NULL;
    jni_handle_slot *slot = &table->slots[parts->index];
    if (!slot->live) return NULL;
    if ((slot->generation & 0x00ffffffu) != parts->generation) return NULL;
    return slot;
}

static bool slot_pin(jni_handle_table *table, jni_handle_slot *slot,
                     uint32_t object_ref)
{
    if (object_ref == 0) return false;
    if (table->cb.pin && !table->cb.pin(table->cb.ctx, object_ref)) return false;
    slot->pinned = true;
    ++table->pinned_count;
    return true;
}

static void slot_unpin(jni_handle_table *table, jni_handle_slot *slot)
{
    if (!slot->pinned) return;
    if (table->cb.unpin && slot->object_ref != 0)
        table->cb.unpin(table->cb.ctx, slot->object_ref);
    slot->pinned = false;
    if (table->pinned_count > 0) --table->pinned_count;
}

static jni_handle_slot *slot_alloc_in_frame(jni_handle_table *table,
                                            jni_ref_kind kind,
                                            uint32_t object_ref, void *cls,
                                            uint32_t frame_id)
{
    if (!table || table->capacity == 0) return NULL;
    if (table->live_count >= table->capacity) return NULL;
    if (kind == JNI_REF_LOCAL && frame_id == 0) return NULL;
    for (uint32_t n = 0; n < table->capacity; ++n) {
        uint32_t i = (table->next_hint + n) % table->capacity;
        jni_handle_slot *slot = &table->slots[i];
        if (slot->live) continue;
        /* generation 必须在槽位复用后继续递增：slot_release() 已加 1，
         * 这里不能把它清零，否则旧 token 会在同一 generation 下重新有效。 */
        uint32_t generation = slot->generation & 0x00ffffffu;
        memset(slot, 0, sizeof(*slot));
        slot->generation = generation;
        slot->object_ref = object_ref;
        slot->cls = cls;
        slot->vm_tag = table->vm_tag;
        slot->kind = (uint8_t)kind;
        if (kind == JNI_REF_LOCAL) slot->frame_id = frame_id;
        if (object_ref != 0 && (kind == JNI_REF_LOCAL || kind == JNI_REF_GLOBAL)) {
            if (!slot_pin(table, slot, object_ref)) {
                slot->object_ref = 0;
                slot->cls = NULL;
                slot->frame_id = 0;
                slot->kind = JNI_REF_NONE;
                slot->live = false;
                return NULL;
            }
        }
        slot->live = true;
        ++table->live_count;
        table->next_hint = (i + 1u) % table->capacity;
        return slot;
    }
    return NULL;
}

static jni_handle_slot *slot_alloc(jni_handle_table *table,
                                   jni_ref_kind kind, uint32_t object_ref,
                                   void *cls)
{
    return slot_alloc_in_frame(table, kind, object_ref, cls,
                               kind == JNI_REF_LOCAL ? table_current_frame(table) : 0);
}

static jobject token_for_slot(const jni_handle_table *table,
                              const jni_handle_slot *slot)
{
    uint32_t index = (uint32_t)(slot - table->slots);
    jobject token = jni_token_null();
    if (!jni_token_encode(index, slot->generation, table->vm_tag, &token))
        return jni_token_null();
    return token;
}

static void slot_release(jni_handle_table *table, jni_handle_slot *slot)
{
    if (!slot || !slot->live) return;
    slot_unpin(table, slot);
    slot->live = false;
    slot->object_ref = 0;
    slot->cls = NULL;
    slot->frame_id = 0;
    slot->generation = (slot->generation + 1u) & 0x00ffffffu;
    if (table->live_count > 0) --table->live_count;
}

bool jni_handles_init(jni_handle_table *table, uint32_t capacity,
                      uint8_t vm_tag, const jni_handle_callbacks *cb)
{
    if (!table || capacity == 0) return false;
    memset(table, 0, sizeof(*table));
    table->slots = calloc(capacity, sizeof(*table->slots));
    if (!table->slots) return false;
    table->capacity = capacity;
    table->vm_tag = vm_tag;
    if (cb) table->cb = *cb;
    table->frame_depth = 1;
    table->frame_counter = 1;
    table->frame_stack[0] = 1;
    return true;
}

void jni_handles_shutdown(jni_handle_table *table)
{
    if (!table) return;
    if (table->slots) {
        for (uint32_t i = 0; i < table->capacity; ++i) {
            if (table->slots[i].live) slot_release(table, &table->slots[i]);
        }
        free(table->slots);
        table->slots = NULL;
    }
    memset(table, 0, sizeof(*table));
}

jobject jni_handle_new_local(jni_handle_table *table, uint32_t object_ref)
{
    if (object_ref == 0) return jni_token_null();
    jni_handle_slot *slot = slot_alloc(table, JNI_REF_LOCAL, object_ref, NULL);
    return slot ? token_for_slot(table, slot) : jni_token_null();
}

jobject jni_handle_new_global(jni_handle_table *table, uint32_t object_ref)
{
    if (object_ref == 0) return jni_token_null();
    jni_handle_slot *slot = slot_alloc(table, JNI_REF_GLOBAL, object_ref, NULL);
    return slot ? token_for_slot(table, slot) : jni_token_null();
}

jobject jni_handle_new_weak(jni_handle_table *table, uint32_t object_ref)
{
    if (object_ref == 0) return jni_token_null();
    jni_handle_slot *slot = slot_alloc(table, JNI_REF_WEAK, object_ref, NULL);
    return slot ? token_for_slot(table, slot) : jni_token_null();
}

jobject jni_handle_new_class(jni_handle_table *table, void *cls)
{
    if (!cls) return jni_token_null();
    jni_handle_slot *slot = slot_alloc(table, JNI_REF_LOCAL, 0, cls);
    return slot ? token_for_slot(table, slot) : jni_token_null();
}

jobject jni_handle_new_local_in_frame(jni_handle_table *table,
                                      uint32_t object_ref, uint32_t frame_id)
{
    if (object_ref == 0 || frame_id == 0) return jni_token_null();
    jni_handle_slot *slot = slot_alloc_in_frame(table, JNI_REF_LOCAL,
                                                object_ref, NULL, frame_id);
    return slot ? token_for_slot(table, slot) : jni_token_null();
}

jobject jni_handle_new_class_in_frame(jni_handle_table *table, void *cls,
                                      uint32_t frame_id)
{
    if (!cls || frame_id == 0) return jni_token_null();
    jni_handle_slot *slot = slot_alloc_in_frame(table, JNI_REF_LOCAL,
                                                0, cls, frame_id);
    return slot ? token_for_slot(table, slot) : jni_token_null();
}

bool jni_handle_delete_local_in_frame(jni_handle_table *table, jobject token,
                                      uint32_t current_frame)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot || slot->kind != JNI_REF_LOCAL) return false;
    if (current_frame == 0 || slot->frame_id != current_frame) return false;
    slot_release(table, slot);
    return true;
}

void jni_handle_pop_frame_slots(jni_handle_table *table, uint32_t frame_id)
{
    if (table && frame_id != 0) pop_frame_slots(table, frame_id);
}

jobject jni_handle_promote_to_frame(jni_handle_table *table, jobject token,
                                    uint32_t target_frame)
{
    if (!table || target_frame == 0 || jni_token_is_null(token))
        return jni_token_null();
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind actual;
    if (!jni_handle_resolve_any(table, token, &ref, &cls, &actual))
        return jni_token_null();
    if (cls) return jni_handle_new_class_in_frame(table, cls, target_frame);
    return jni_handle_new_local_in_frame(table, ref, target_frame);
}

bool jni_handle_resolve_object(jni_handle_table *table, jobject token,
                               jni_ref_kind accept, uint32_t *object_ref,
                               jni_ref_kind *actual)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot) return false;
    if (slot->cls != NULL) return false;
    if (slot->kind != (uint8_t)accept) return false;
    if (slot->kind == JNI_REF_CLASS || slot->kind == JNI_REF_NONE) return false;
    if (object_ref) *object_ref = slot->object_ref;
    if (actual) *actual = (jni_ref_kind)slot->kind;
    return true;
}

bool jni_handle_resolve_class(jni_handle_table *table, jobject token,
                              void **cls)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot || slot->cls == NULL) return false;
    if (slot->kind != JNI_REF_LOCAL && slot->kind != JNI_REF_GLOBAL &&
        slot->kind != JNI_REF_WEAK) return false;
    if (cls) *cls = slot->cls;
    return true;
}

bool jni_handle_resolve_any(jni_handle_table *table, jobject token,
                            uint32_t *object_ref, void **cls,
                            jni_ref_kind *actual)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot) return false;
    if (slot->kind != JNI_REF_LOCAL && slot->kind != JNI_REF_GLOBAL &&
        slot->kind != JNI_REF_WEAK) return false;
    if (slot->cls == NULL && slot->object_ref == 0) return false;
    if (object_ref) *object_ref = slot->object_ref;
    if (cls) *cls = slot->cls;
    if (actual) *actual = (jni_ref_kind)slot->kind;
    return true;
}

bool jni_handle_delete_local(jni_handle_table *table, jobject token)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot || slot->kind != JNI_REF_LOCAL) return false;
    if (slot->frame_id != table_current_frame(table)) return false;
    slot_release(table, slot);
    return true;
}

bool jni_handle_delete_local_any(jni_handle_table *table, jobject token)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot || slot->kind != JNI_REF_LOCAL) return false;
    slot_release(table, slot);
    return true;
}

bool jni_handle_delete_global(jni_handle_table *table, jobject token)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot || slot->kind != JNI_REF_GLOBAL) return false;
    slot_release(table, slot);
    return true;
}

bool jni_handle_delete_weak(jni_handle_table *table, jobject token)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot || slot->kind != JNI_REF_WEAK) return false;
    slot_release(table, slot);
    return true;
}

bool jni_handle_delete(jni_handle_table *table, jobject token)
{
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot) return false;
    switch (slot->kind) {
    case JNI_REF_LOCAL:  return jni_handle_delete_local(table, token);
    case JNI_REF_GLOBAL: return jni_handle_delete_global(table, token);
    case JNI_REF_WEAK:   return jni_handle_delete_weak(table, token);
    default:             return false;
    }
}

jobject jni_handle_new_local_ref(jni_handle_table *table, jobject token)
{
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind actual;
    if (!jni_handle_resolve_any(table, token, &ref, &cls, &actual))
        return jni_token_null();
    if (cls) return jni_handle_new_class(table, cls);
    return jni_handle_new_local(table, ref);
}

jobject jni_handle_new_global_ref(jni_handle_table *table, jobject token)
{
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind actual;
    if (!jni_handle_resolve_any(table, token, &ref, &cls, &actual))
        return jni_token_null();
    if (cls) {
        jni_handle_slot *slot = slot_alloc(table, JNI_REF_GLOBAL, 0, cls);
        return slot ? token_for_slot(table, slot) : jni_token_null();
    }
    return jni_handle_new_global(table, ref);
}

jboolean jni_handle_is_same(jni_handle_table *table, jobject a, jobject b)
{
    if (jni_token_is_null(a) || jni_token_is_null(b))
        return (jni_token_is_null(a) && jni_token_is_null(b)) ? JNI_TRUE : JNI_FALSE;
    uint32_t ra = 0, rb = 0;
    void *ca = NULL, *cb = NULL;
    jni_ref_kind ka, kb;
    if (!jni_handle_resolve_any(table, a, &ra, &ca, &ka)) return JNI_FALSE;
    if (!jni_handle_resolve_any(table, b, &rb, &cb, &kb)) return JNI_FALSE;
    if (ca != NULL || cb != NULL) return (ca == cb) ? JNI_TRUE : JNI_FALSE;
    return (ra == rb) ? JNI_TRUE : JNI_FALSE;
}

jobjectRefType jni_handle_ref_type(jni_handle_table *table, jobject token)
{
    if (jni_token_is_null(token)) return JNIInvalidRefType;
    jni_token_parts parts;
    jni_handle_slot *slot = slot_from_token(table, token, &parts);
    if (!slot) return JNIInvalidRefType;
    switch (slot->kind) {
    case JNI_REF_LOCAL:  return JNILocalRefType;
    case JNI_REF_GLOBAL: return JNIGlobalRefType;
    case JNI_REF_WEAK:   return JNIWeakGlobalRefType;
    default:             return JNIInvalidRefType;
    }
}

uint32_t jni_handle_frame_push(jni_handle_table *table)
{
    if (!table || table->frame_depth >= JNI_HANDLE_MAX_FRAMES) return 0;
    uint32_t next = table->frame_counter + 1u;
    if (next == 0) next = 1;
    table->frame_counter = next;
    table->frame_stack[table->frame_depth++] = next;
    return next;
}

static void pop_frame_slots(jni_handle_table *table, uint32_t frame_id)
{
    for (uint32_t i = 0; i < table->capacity; ++i) {
        jni_handle_slot *slot = &table->slots[i];
        if (slot->live && slot->kind == JNI_REF_LOCAL && slot->frame_id == frame_id)
            slot_release(table, slot);
    }
}

bool jni_handle_frame_pop(jni_handle_table *table, uint32_t frame_id)
{
    if (!table || table->frame_depth <= 1) return false;
    if (table->frame_stack[table->frame_depth - 1] != frame_id) return false;
    pop_frame_slots(table, frame_id);
    --table->frame_depth;
    return true;
}

bool jni_handle_frame_pop_with_result(jni_handle_table *table, uint32_t frame_id,
                                      jobject result, jobject *out)
{
    if (!table || table->frame_depth <= 1) return false;
    if (table->frame_stack[table->frame_depth - 1] != frame_id) return false;
    jobject promoted = jni_token_null();
    if (!jni_token_is_null(result)) {
        jni_token_parts parts;
        jni_handle_slot *slot = slot_from_token(table, result, &parts);
        if (!slot || slot->kind != JNI_REF_LOCAL || slot->frame_id != frame_id)
            return false;
        uint32_t parent_frame = table->frame_stack[table->frame_depth - 2];
        promoted = jni_handle_promote_to_frame(table, result, parent_frame);
        if (jni_token_is_null(promoted)) return false;
    }
    pop_frame_slots(table, frame_id);
    --table->frame_depth;
    if (out) *out = promoted;
    return true;
}

uint32_t jni_handle_live_count(const jni_handle_table *table)
{
    return table ? table->live_count : 0;
}

uint32_t jni_handle_pinned_count(const jni_handle_table *table)
{
    return table ? table->pinned_count : 0;
}

uint32_t jni_handle_current_frame(const jni_handle_table *table)
{
    return table_current_frame(table);
}
