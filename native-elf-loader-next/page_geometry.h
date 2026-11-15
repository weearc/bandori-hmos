#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host page size is cached once. A zero value is an explicit platform error. */
size_t host_page_size(void);
size_t guest_page_align(void);
void page_geometry_configure(const char *directory);
const char *page_geometry_status(void);
int page_align_down(uint64_t value,uint64_t align,uint64_t *out);
int page_align_up(uint64_t value,uint64_t align,uint64_t *out);
/* Reserve an aligned PROT_NONE range without touching adjacent mappings. */
void *page_reserve(size_t bytes,size_t align);
#ifdef __cplusplus
}
#endif
