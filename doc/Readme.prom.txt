prom module - Prometheus exporter

The 'prom' module exposes the metrics registry (core/AmStatistics)
over HTTP in the Prometheus text exposition format, so a Prometheus
(or VictoriaMetrics, Telegraf, ...) server can scrape SEMS directly.
It owns no metrics of its own: what it serves is whatever the core
and the loaded modules have registered - session and call counters,
SIP transaction counters, codec usage, memory, uptime, plus metrics
of any module using the registry.

Without this module the registry stays invisible to the outside: it
is still maintained (a few atomic increments per event), but nothing
renders it.  Loading it is a runtime choice (add 'prom' to
load_plugins in sems.conf), there is no compile-time switch.

Endpoints
---------
GET /metrics   - the whole registry, one scrape
HEAD /metrics  - same without the body

The response body follows the exposition format:

  # TYPE sems_core_session_num gauge
  # HELP sems_core_session_num active sessions
  sems_core_session_num 3
  # TYPE sems_core_codec_usage gauge
  # HELP sems_core_codec_usage number of media streams per codec, by direction
  sems_core_codec_usage{codec="pcmu", direction="read"} 2

Metric names are "<prefix>_<group>", where <group> is the registry
name ("core_session_num" etc.) and prefix defaults to "sems".  The
type and the help text come from what the metric's owner registered
(setHelp()); metrics without a help text get no "# HELP" line.  Per
series labels come from the counter's labels; global labels added by
configuration (see below) are merged into every series.

Configuration (etc/prom.conf)
-----------------------------
listen_ip      - address to bind to; default 127.0.0.1.  The exporter
                 is meant to be scraped locally or through a local
                 proxy; bind to a routable address only together
                 with allow_network.
listen_port    - TCP port; default 9042.
prefix         - metric name prefix; default "sems".
cache_ttl      - seconds a rendered response is reused; default 1,
                 0 disables caching (see Caching below).
allow_network  - comma separated CIDR networks allowed to scrape,
                 IPv4 and IPv6 ("127.0.0.0/8,fd00::/8"); a bare
                 address means a single host.  Empty (the default)
                 allows any peer.
label.<name>   - a global label added to every metric, one key per
                 label ("label.node=sems-1"), because the module
                 configuration reader keeps one value per key.

Invalid values (bad port, bad CIDR entry) or a failed bind make the
module refuse to load, with the reason in the log; the core treats a
plug-in that fails to load as fatal, so SEMS reports the failure and
exits.  On success the module logs, at info level, the address it
listens on and the effective configuration.

Caching
-------
Rendering walks the whole registry under the group locks and
evaluates the computed metrics (/proc reads, table scans), so the
rendered response is cached for cache_ttl seconds and requests
inside the window are answered from the cache.  Many scrapers cost
SEMS one render per second.  A gauge may therefore read up to
cache_ttl seconds stale; a Prometheus scrape (interval >= 15 s,
values stamped with the response time) cannot notice, but do not
use this endpoint as a real-time source.

Access control
--------------
When allow_network is set, the TCP peer of every request is checked
against it before the cache is consulted; a peer outside every
network gets 403.  The check is on the TCP address: behind a
reverse proxy all scrapers appear as the proxy's address, so filter
at the proxy instead (X-Forwarded-For is not interpreted).

Performance
-----------
The HTTP server (evhttp) runs on its own thread over one event
loop, which serves all connections, so the exporter is able to
accept and serve many scrapes in parallel without affecting call
processing; with caching enabled, the cost of scraping for SEMS
is one registry render per cache_ttl seconds regardless of the
request rate.  Nothing in the call path is added by loading this
module.

Relation to other statistics channels
-------------------------------------
The "statistics" DI interface (part of the core, works without
this module) reads the same registry per request, without caching
or HTTP: use it for ad hoc queries and scripts, prom for
continuous monitoring.  Methods:

  dump()             - all metrics, one entry per series
  get(<metric>)      - a group name, or one series with its labels
                       ("core_codec_usage{codec=pcmu,...}")
  meta(<metric>)     - {type, help} of a metric
  reset(<metric>)    - zero the counters, if their owner allows it

Invocations: "DI statistics dump" over the UDP stats socket (see
etc/stats.conf), statistics.dump over jsonrpc, di("statistics",
"dump") over xmlrpc2di.  Replies are flat {metric: number} maps,
labels embedded in the key; unlike prom they carry no global
labels and no metric prefix.

The 'stats' module, which provides that socket, also serves its
own older per-event statistics counters - those are unrelated to
the registry.