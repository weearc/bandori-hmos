#pragma once
#include "dvm/dvm_internal.h"

/* java.util.PriorityQueue 的真实数组二叉最小堆子集。
 *
 * 元素比较只通过 DVM 调用完成：自然序调用元素的 compareTo(Object)I（或标准
 * 包装类型/String/Enum 的类型化 compareTo），显式 Comparator 调用其
 * compare(Object,Object)I。没有按指针、哈希或插入顺序的假排序。
 *
 * 队列数组的顺序就是堆数组顺序，不是遍历顺序；iterator()/toArray() 暴露的
 * 也是该顺序，与 JDK 一致。 */
void priority_queue_vm_start(struct dvm *vm);

bool priority_queue_call(struct dvm *vm, const char *cls, const char *method,
                         const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out);

bool priority_queue_selftest(struct dvm *vm);
