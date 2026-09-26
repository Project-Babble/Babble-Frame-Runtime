// A plugin that speaks a future ABI version: the runtime must refuse it.
#include <babble/output_plugin.h>

namespace {
void *create(const babble_host_v1 *, const char *, const char *) { return nullptr; }
int start(void *) { return 0; }
void stop(void *) {}
void destroy(void *) {}
const babble_output_v1 kPlugin = {BABBLE_OUTPUT_ABI + 1, "bad_abi", "0", create, start, stop, destroy};
} // namespace

extern "C" BABBLE_PLUGIN_EXPORT const babble_output_v1 *babble_output_plugin_v1(void) { return &kPlugin; }
