#ifndef ANDROID_SIGNAL_SHIM_H
#define ANDROID_SIGNAL_SHIM_H
#ifdef __cplusplus
extern "C" {
#endif
/* Android LP64 guest objects: sigset=8 bytes; sigaction=32 bytes.
 * SA_SIGINFO and user restorer are not supported on installation. */
int android_signal_sigemptyset(void *guest_set);
int android_signal_sigaddset(void *guest_set, int signal_number);
int android_signal_sigaction(int signal_number, const void *guest_action, void *guest_old_action);
int android_signal_pthread_sigmask(int how, const void *guest_set, void *guest_old_set);
#ifdef __cplusplus
}
#endif
#endif
