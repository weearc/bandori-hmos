typedef __SIZE_TYPE__ size_t;
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern size_t strlen(const char *);
extern int snprintf(char *, size_t, const char *, ...);

/* Step codes: 0 success, 1 invalid output, 2 allocation, 3 memcpy,
   4 strlen, 5 snprintf length, 6 snprintf content. */
__attribute__((visibility("default")))
int probe_libc_run(unsigned long long guard, unsigned long long *observed) {
    if (!observed) return 1;
    *observed = 0;
    char *p = (char *)malloc(64);
    if (!p) return 2;
    const char source[] = "guest-libc";
    if (memcpy(p, source, sizeof(source)) != p) { free(p); return 3; }
    for (size_t i = 0; i < sizeof(source); ++i) if (p[i] != source[i]) { free(p); return 3; }
    if (strlen(p) != 10) { free(p); return 4; }
    char result[64];
    int n = snprintf(result, sizeof(result), "%s:%016llx", p, guard);
    if (n != 27) { free(p); return 5; }
    const char prefix[] = "guest-libc:";
    for (size_t i = 0; i < sizeof(prefix)-1; ++i) if (result[i] != prefix[i]) { free(p); return 6; }
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) if (result[11+i] != hex[(guard >> ((15-i)*4)) & 15]) { free(p); return 6; }
    if (result[27] != 0) { free(p); return 6; }
    *observed = guard ^ 0xa55aa55a12345678ULL;
    free(p);
    return 0;
}
