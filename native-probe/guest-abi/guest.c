/* Freestanding Android-target ARM64 ELF. No Android libc or game code. */
static int constructor_state;
__attribute__((constructor)) static void init(void) { constructor_state = 0x1357; }
__attribute__((visibility("default"))) int probe_android_ctor_state(void) { return constructor_state; }
__attribute__((visibility("default"))) unsigned long long probe_android_integer(unsigned long long a, unsigned long long b) { return (a ^ b) + 17ULL; }
__attribute__((visibility("default"))) double probe_android_float(double a, double b) { return a * 2.0 + b; }
