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
/** @file prom.h - Prometheus text exposition over HTTP */

#ifndef __prom_h__
#define __prom_h__

#include "AmApi.h"
#include "AmThread.h"

#include <event2/event.h>
#include <event2/http.h>

#include <sys/socket.h>

#include <string>
#include <vector>
using std::string;
using std::vector;

/** one allow_network CIDR entry, parsed in prom.cpp */
struct AllowedNetwork {
  int family;
  unsigned char bytes[16];
  unsigned int mask_len;

  bool contains(const sockaddr_storage& addr) const;
};

/** HTTP server rendering the metrics registry in the Prometheus text
 *  format; runs its event base on a separate thread */
class Prom : public AmThread {
  static Prom* _instance;

  string ip;
  int port;
  string prefix;
  int cache_ttl;
  vector<AllowedNetwork> allow_networks;

  /** rendered response, reused while fresh; evhttp runs all callbacks on
   *  the event loop thread, so the cache needs no lock */
  string cached_body;
  long long cache_valid_until;

  struct event_base* ev_base;
  struct evhttp* ev_http;

  int loadConfig();
  int init();
  void metricsRequest(struct evhttp_request* req);
  void renderMetrics(evbuffer* buf);

public:
  Prom();
  ~Prom() override;

  /** creates, configures and binds the server; on failure returns null */
  static Prom* instance();
  static void dispose();

  void run() override;
  void on_stop() override;
};

#endif