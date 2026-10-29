// tdlib-purple's entry point as a static protocol has C++ linkage
// (PURPLE_INIT_PLUGIN in a C++ file): called through this for purple-host.c
#include <glib.h>

gboolean purple_init_telegram_tdlib_plugin(void);

extern "C" gboolean purple_host_init_telegram(void)
{
	return purple_init_telegram_tdlib_plugin();
}
