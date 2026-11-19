/* Host-side callbacks referenced by the runtime overlays.
 *
 * The HarmonyOS host (the NAPI payload layer) owns foreground/destroy state.
 * These are weak definitions so a host build that provides strong symbols
 * wins, while a standalone build of only the runtime still links.
 */
#include <stdbool.h>

__attribute__((weak)) int sdk_app_vm_is_foreground(void) { return 1; }
__attribute__((weak)) int sdk_app_vm_should_destroy(void) { return 0; }
