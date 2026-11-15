/*
 * Runtime-level class contracts for contracts the application DEX cannot
 * declare.  See runtime_contract.h for lifecycle and ownership.
 *
 * The candidate build includes next_interpose.h before every translation unit
 * (except next_arena.c and tls_client.c), so malloc/calloc/realloc/free are
 * redirected to the VM arena.  This file relies on that: dvm.c allocates
 * java.lang.String's ifaces with calloc(), grows it with realloc() here, and
 * dvm_destroy() frees it with free(), all through the same arena allocator.
 * Do not free the old ifaces block after a successful realloc: realloc has
 * already released or moved it and struct dvm_class owns the returned block.
 */

#include "runtime_contract.h"
#include "dvm/dvm_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

static bool class_desc_is(const struct dvm_class *cls, const char *desc)
{
   return cls && cls->desc && desc && !strcmp(cls->desc, desc);
}

static bool iface_present(const struct dvm_class *cls,
                          const struct dvm_class *iface,
                          const char *iface_desc)
{
   for (int i = 0; i < cls->nifaces; ++i) {
      if (cls->ifaces[i] == iface) return true;
      if (class_desc_is(cls->ifaces[i], iface_desc)) return true;
   }
   return false;
}

bool runtime_contract_vm_start(struct dvm *vm)
{
   if (!vm) return false;

   struct dvm_class *string_cls =
      dvm_find_class_or_external(vm, "java/lang/String");
   struct dvm_class *char_sequence_cls =
      dvm_find_class_or_external(vm, "java/lang/CharSequence");
   struct dvm_class *comparable_cls = dvm_find_class_or_external(vm, "java/lang/Comparable");
   if (!string_cls || !char_sequence_cls || !comparable_cls) {
      sdk_trace_fprintf(stderr,
                        "RUNTIME_CONTRACT FAIL string_or_charsequence_unavailable\n");
      return false;
   }

   /*
    * java.lang.String is the boot-class placeholder that the application DEX
    * uses but cannot define.  A real device has String implement
    * CharSequence; none of the DEX files in this workspace define String, so
    * no interface edge exists unless it is installed here.
    */
   struct dvm_class *interfaces[] = {char_sequence_cls, comparable_cls};
   const char *descriptors[] = {"Ljava/lang/CharSequence;", "Ljava/lang/Comparable;"};
   for (int contract = 0; contract < 2; contract++) {
   interfaces[contract]->is_interface = true;
   if (iface_present(string_cls, interfaces[contract], descriptors[contract])) continue;

   if (string_cls->nifaces < 0) {
      sdk_trace_fprintf(stderr,
                        "RUNTIME_CONTRACT FAIL negative_iface_count\n");
      return false;
   }

   size_t count = (size_t)string_cls->nifaces + 1u;
   if (count > (size_t)INT32_MAX ||
       count > SIZE_MAX / sizeof *string_cls->ifaces) {
      sdk_trace_fprintf(stderr,
                        "RUNTIME_CONTRACT FAIL iface_count_overflow\n");
      return false;
   }

   struct dvm_class **grown =
      realloc(string_cls->ifaces, count * sizeof *grown);
   if (!grown) {
      /* realloc leaves the old block owned by the class in place. */
      sdk_trace_fprintf(stderr,
                        "RUNTIME_CONTRACT FAIL iface_realloc\n");
      return false;
   }

   grown[string_cls->nifaces] = interfaces[contract];
   string_cls->ifaces = grown;
   string_cls->nifaces++;
   }

   /*
    * The external placeholder is an interface on a real device.  This flag is
    * not consulted by class_assignable() (the interface edge above is what
    * fixes the cast); setting it keeps the class metadata truthful for code
    * that inspects is_interface.
    */
   char_sequence_cls->is_interface = true;

   sdk_trace_fprintf(stderr,
                     "RUNTIME_CONTRACT String->CharSequence,Comparable installed nifaces=%d\n",
                     string_cls->nifaces);
   return true;
}
