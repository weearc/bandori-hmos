#pragma once
#include "dvm/dvm_internal.h"
#include <stddef.h>

/* 原 APK 签名证书 runtime。
 *
 * 证书来源：BanGDream_10.1.3.0.0.0.0_20260917_031717_9932d.apk
 *           META-INF/BILIGAME.RSA（PKCS#7 中的 X.509 证书 DER）
 * DER 长度：715
 * SHA256  ：8d9975b308c79d52b1703900afe35f39afd5d40a4d01c8d29064288078a7af7d
 * MD5     ：4502a02a00395dec05a4134ad593224d
 *
 * 初始化校验内嵌 DER 的长度和完整 SHA256；校验失败 fail-closed，不注册伪证书。
 */
bool apk_signature_vm_start(struct dvm *vm);

/* 分发 android/content/pm/Signature 的实例方法；未匹配返回 false。 */
bool apk_signature_call(struct dvm *vm, const char *class_name, const char *method,
                        const char *sig, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out);

/* 返回内嵌 DER 指针；len 非空时写入 715。 */
const unsigned char *apk_signature_der(size_t *len);

/* 用给定 DER 创建新的 Signature；字节被复制，不与调用方共享。
 * 失败返回 0 并抛出异常。 */
dvm_ref apk_signature_new(struct dvm *vm, const unsigned char *der, size_t len);
