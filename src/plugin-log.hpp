#pragma once

#include <util/base.h>

#define plugin_log(level, format, ...) blog(level, "[obs-srtla-source] " format, ##__VA_ARGS__)
