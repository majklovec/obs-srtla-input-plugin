// Counter storage for the embedded irlserver/srtla receiver.
//
// Upstream defines these in metrics/prometheus.cpp next to an HTTP scrape
// server. The plugin never starts that server, so only the counters are built.

#include <cstdint>

#include "metrics/prometheus.h"

namespace srtla::metrics {

namespace {
uint64_t g_values[COUNTER_COUNT] = {};
}

void inc(Counter c, uint64_t n)
{
	g_values[c] += n;
}

uint64_t value(Counter c)
{
	return g_values[c];
}

} // namespace srtla::metrics
