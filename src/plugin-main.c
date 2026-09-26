#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-srtla-source", "en-US")

extern void srtla_source_register(void);

bool obs_module_load(void)
{
	srtla_source_register();
	blog(LOG_INFO, "[obs-srtla-source] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-srtla-source] plugin unloaded");
}
