/*
 * Copyright (C) 2026 Michael Furmur (yeti-switch/sems)
 * Copyright (C) 2026 Vadim Saranov
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "prom.h"

#include "AmStatistics.h"
#include "sip/ip_util.h"

#include <event2/buffer.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>

#define DEFAULT_LISTEN_IP   "127.0.0.1"
#define DEFAULT_LISTEN_PORT 9042
#define DEFAULT_PREFIX      "sems"
#define DEFAULT_CACHE_TTL   1

bool AllowedNetwork::contains(const sockaddr_storage& addr) const
{
  if (addr.ss_family != family)
    return false;

  const unsigned char* p = family == AF_INET
    ? (const unsigned char*)&SAv4(&addr)->sin_addr
    : (const unsigned char*)&SAv6(&addr)->sin6_addr;

  for (unsigned int i = 0; i < mask_len / 8; i++)
    if (p[i] != bytes[i])
      return false;

  unsigned int rest = mask_len % 8;
  if (rest)
    if ((p[mask_len / 8] & (0xff << (8 - rest))) != (bytes[mask_len / 8] & (0xff << (8 - rest))))
      return false;

  return true;
}

/** "10.0.0.0/8", bare address means a single host */
static bool parseNetwork(const string& s, AllowedNetwork& net)
{
  size_t slash = s.find('/');
  string addr_str = slash == string::npos ? s : s.substr(0, slash);

  sockaddr_storage addr;
  if (!am_inet_pton(addr_str.c_str(), &addr))
    return false;

  net.family = addr.ss_family;
  if (net.family == AF_INET) {
    memcpy(net.bytes, &SAv4(&addr)->sin_addr, 4);
    net.mask_len = slash == string::npos ? 32 : 0;
  } else {
    memcpy(net.bytes, &SAv6(&addr)->sin6_addr, 16);
    net.mask_len = slash == string::npos ? 128 : 0;
  }

  if (slash != string::npos) {
    /* int overload: the unsigned one returns true on error */
    int len;
    if (!str2int(s.substr(slash + 1), len) || len < 0)
      return false;
    if ((unsigned)len > (net.family == AF_INET ? 32u : 128u))
      return false;
    net.mask_len = len;
  }

  return true;
}

Prom* Prom::_instance = nullptr;

Prom::Prom()
  : ip(DEFAULT_LISTEN_IP)
  , port(DEFAULT_LISTEN_PORT)
  , prefix(DEFAULT_PREFIX)
  , cache_ttl(DEFAULT_CACHE_TTL)
  , cache_valid_until(0)
  , ev_base(nullptr)
  , ev_http(nullptr)
{
}

Prom::~Prom()
{
  if (ev_http)
    evhttp_free(ev_http);
  if (ev_base)
    event_base_free(ev_base);
}

Prom* Prom::instance()
{
  if (!_instance) {
    _instance = new Prom();
    if (_instance->loadConfig() || _instance->init()) {
      delete _instance;
      _instance = nullptr;
    } else {
      _instance->start();
    }
  }
  return _instance;
}

void Prom::dispose()
{
  if (!_instance)
    return;
  _instance->stop();
  _instance->join();
  delete _instance;
  _instance = nullptr;
}

/** config keys "label.<name>" become global registry labels;
 *  AmConfigReader keeps one value per key, hence the dotted suffix */
int Prom::loadConfig()
{
  AmConfigReader cfg;
  string conf_file = add2path(AmConfig::ModConfigPath, 1, MOD_NAME ".conf");
  if (!file_exists(conf_file)) {
    INFO("configuration file '%s' not found. using defaults.\n", conf_file.c_str());
  } else if (cfg.loadFile(conf_file)) {
    return -1;
  }

  ip = cfg.getParameter("listen_ip", DEFAULT_LISTEN_IP);
  port = cfg.getParameterInt("listen_port", DEFAULT_LISTEN_PORT);
  if (port < 1 || port > 65535) {
    ERROR("invalid listen_port %d\n", port);
    return -1;
  }
  prefix = cfg.getParameter("prefix", DEFAULT_PREFIX);
  cache_ttl = cfg.getParameterInt("cache_ttl", DEFAULT_CACHE_TTL);
  if (cache_ttl < 0) {
    ERROR("invalid cache_ttl %d\n", cache_ttl);
    return -1;
  }

  for (auto& entry : explode(trim(cfg.getParameter("allow_network"), " \t"), ",")) {
    if (entry.empty())
      continue;
    AllowedNetwork net;
    if (!parseNetwork(entry, net)) {
      ERROR("invalid allow_network entry '%s'\n", entry.c_str());
      return -1;
    }
    allow_networks.push_back(net);
  }

  for (auto& kv : cfg) {
    static const string label_prefix = "label.";
    if (kv.first.compare(0, label_prefix.size(), label_prefix) != 0)
      continue;
    AmStatistics::instance()->addLabel(kv.first.substr(label_prefix.size()), kv.second);
  }

  return 0;
}

int Prom::init()
{
  ev_base = event_base_new();
  if (!ev_base) {
    ERROR("couldn't create an event_base\n");
    return -1;
  }

  ev_http = evhttp_new(ev_base);
  if (!ev_http) {
    ERROR("couldn't create evhttp\n");
    return -1;
  }

  evhttp_set_allowed_methods(ev_http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD);
  evhttp_set_cb(
    ev_http, "/metrics",
    [](struct evhttp_request* req, void* arg) {
      static_cast<Prom*>(arg)->metricsRequest(req);
    },
    this);

  if (!evhttp_bind_socket_with_handle(ev_http, ip.c_str(), static_cast<unsigned short>(port))) {
    int err = errno;
    ERROR("couldn't bind prometheus exporter to %s:%d: %s\n", ip.c_str(), port, strerror(err));
    return -1;
  }

  INFO("prometheus exporter listening on http://%s:%d/metrics "
       "(prefix=%s, cache_ttl=%d, allow_network=%s)\n",
       ip.c_str(), port, prefix.c_str(), cache_ttl,
       allow_networks.empty() ? "any" : "restricted");

  return 0;
}

static void serializeLabel(evbuffer* buf, const map<string, string>::value_type& label, bool& first)
{
  evbuffer_add_printf(buf, first ? "%s=\"%s\"" : ", %s=\"%s\"",
                      label.first.c_str(), label.second.c_str());
  first = false;
}

void Prom::metricsRequest(struct evhttp_request* req)
{
  if (!allow_networks.empty()) {
    struct evhttp_connection* conn = evhttp_request_get_connection(req);
    char* addr = nullptr;
    ev_uint16_t peer_port = 0;
    sockaddr_storage peer;
    if (conn)
      evhttp_connection_get_peer(conn, &addr, &peer_port);
    /* libevent owns the address string, do not free it */
    if (!addr || !am_inet_pton(addr, &peer)) {
      evhttp_send_reply_start(req, 403, "Forbidden");
      evhttp_send_reply_end(req);
      return;
    }

    bool allowed = false;
    for (auto& net : allow_networks)
      if (net.contains(peer)) {
        allowed = true;
        break;
      }
    if (!allowed) {
      DBG("metrics request from %s denied by allow_network", am_inet_ntop(&peer).c_str());
      evhttp_send_reply_start(req, 403, "Forbidden");
      evhttp_send_reply_end(req);
      return;
    }
  }

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long long now = ts.tv_sec;

  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/plain; version=0.0.4");

  evbuffer* buf = evbuffer_new();

  if (cache_ttl && now < cache_valid_until) {
    evbuffer_add(buf, cached_body.data(), cached_body.size());
  } else {
    renderMetrics(buf);
    if (cache_ttl) {
      size_t len = evbuffer_get_length(buf);
      cached_body.resize(len);
      evbuffer_copyout(buf, &cached_body[0], len);
      cache_valid_until = now + cache_ttl;
    }
  }

  evhttp_send_reply_start(req, HTTP_OK, "OK");
  evhttp_send_reply_chunk(req, buf);
  evhttp_send_reply_end(req);
  evbuffer_free(buf);
}

void Prom::renderMetrics(evbuffer* buf)
{
  /** global registry labels change at module load only, so the reference
   *  stays valid for the whole request */
  const map<string, string>& global_labels = AmStatistics::instance()->getLabelsRef();

  AmStatistics::instance()->iterateGroups(
    [this, buf, &global_labels](const string& name, StatCountersGroupsInterface& group) {
      evbuffer_add_printf(buf, "# TYPE %s_%s %s\n", prefix.c_str(), name.c_str(),
                          StatCountersGroupsInterface::type2str(group.getType()));

      const string& help = group.getHelp();
      if (!help.empty())
        evbuffer_add_printf(buf, "# HELP %s_%s %s\n", prefix.c_str(), name.c_str(), help.c_str());

      group.iterateCounters([this, &global_labels, buf, &name](unsigned long long value,
                                                               const map<string, string>& labels) {
        if (global_labels.empty() && labels.empty()) {
          evbuffer_add_printf(buf, "%s_%s %llu\n", prefix.c_str(), name.c_str(), value);
          return;
        }

        evbuffer_add_printf(buf, "%s_%s{", prefix.c_str(), name.c_str());
        bool first = true;
        for (auto& l : global_labels)
          serializeLabel(buf, l, first);
        for (auto& l : labels)
          serializeLabel(buf, l, first);
        evbuffer_add_printf(buf, "} %llu\n", value);
      });
    });
}

void Prom::run()
{
  event_base_dispatch(ev_base);
}

void Prom::on_stop()
{
  event_base_loopbreak(ev_base);
}

class PromFactory : public AmPluginFactory {
public:
  PromFactory(const string& name) : AmPluginFactory(name) {}
  ~PromFactory() override { Prom::dispose(); }

  int onLoad() override
  {
    if (!Prom::instance()) {
      ERROR("prometheus exporter initialization failed\n");
      return -1;
    }
    return 0;
  }
};

EXPORT_PLUGIN_FACTORY(PromFactory, MOD_NAME);