/*
 * Copyright (C) 2018-2019 Michael Furmur (yeti-switch/sems)
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
/** @file AmRtcpStat.h - RTCP stream statistics, RFC 3550 */

#ifndef __AmRtcpStat_h__
#define __AmRtcpStat_h__

#include "AmThread.h"

#include <sys/time.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

const int MAX_DROPOUT    = 3000;
const int MAX_MISORDER   = 100;
const int MIN_SEQUENTIAL = 2;
/* max number of accounted rx sources (SSRCs) per stream,
 * to bound memory of a stream against SSRC attacks */
const int MAX_RX_STATS   = 10;

/** Aggregated statistic of a numeric value
 *  (mean and variance computed online, Welford's method) */
template <typename T = int> struct MathStat {
  int    n; /* number of samples    */
  T      max;                      /* maximum value        */
  T      min;                      /* minimum value        */
  T      last;                     /* last value           */
  float  mean;                     /* mean                 */
  double variance_multiplied_by_n; /* variance * n         */

  MathStat() { memset(this, 0, sizeof(MathStat<T>)); }

  inline void update(T v)
  {
    float diff;

    last = v;

    if (n++) {
      if (min > v)
        min = v;
      if (max < v)
        max = v;
    } else {
      min = v;
      max = v;
    }

    diff = v - mean;
    mean += diff / n;

    variance_multiplied_by_n += diff * (v - mean);
  }

  inline double sd() const // standard deviation
  {
    if (n == 0)
      return 0;
    return std::sqrt(variance_multiplied_by_n / n);
  }
};

/** Statistic of one direction (rx or tx) for one RTP source (SSRC) */
struct RtcpUnidirectionalStat {
  uint32_t pkt;   /**< total number of packets        */
  uint32_t bytes; /**< total number of payload bytes  */
  unsigned loss;    /**< total number of packets lost     */
  unsigned reorder; /**< total number of out of order packets */
  unsigned dup;     /**< total number of duplicates packets   */

  int rtcp_jitter; /**< RFC 3550 jitter, unscaled by 16 */

  MathStat<long>     rx_delta;         /**< inter-arrival delta statistic (usec)  */
  MathStat<double>   jitter_usec;      /**< local jitter statistic (usec)         */
  MathStat<uint32_t> rtcp_jitter_usec; /**< RTCP jitter statistic (usec)          */

  RtcpUnidirectionalStat();
};

/** Bidirectional (tx + rx per SSRC) statistic of an RTP stream,
 *  with RTCP report counters and receiver info (RFC 3550) */
struct RtcpBidirectionalStat : public AmMutex {
  using RxStatMap = std::unordered_map<unsigned int, RtcpUnidirectionalStat>;

  /** own send statistics: RTP sent with our own SSRC only;
   *  source of the sender counts in our SR (RFC 3550, section 4) */
  RtcpUnidirectionalStat  tx;
  RxStatMap               rx; /**< recv stream statistics   */
  RtcpUnidirectionalStat* current_rx;

  /** relayed passthrough traffic (foreign SSRC), never counted into SR */
  uint32_t relay_tx_pkt, relay_tx_bytes;

  uint32_t rtcp_rr_sent, rtcp_rr_recv;
  uint32_t rtcp_sr_sent, rtcp_sr_recv;

  // receiver info, RFC 3550, section 4, https://tools.ietf.org/html/rfc3550

  uint16_t max_seq;        /* highest seq. number seen */
  uint32_t cycles;         /* shifted count of seq. number cycles */
  uint32_t base_seq;       /* base seq number */
  uint32_t bad_seq;        /* last 'bad' seq number + 1 */
  uint32_t probation;      /* sequ. packets till source is valid */
  uint32_t received;       /* packets received */
  uint32_t expected_prior; /* packet expected at last interval */
  uint32_t received_prior; /* packet received at last interval */
  int32_t  transit;        /* relative trans time for prev pkt */

  timeval rx_recv_time;

  uint32_t total_lost;
  uint8_t  fraction_lost;

  uint32_t sr_lsr; /* last SR timestamp received in a sender report */
  timeval  sr_recv_time;

  MathStat<uint32_t> rtt;                /**< round trip time statistic (usec) */
  MathStat<uint32_t> rtcp_remote_jitter; /**< jitter received in remote reports */

  void init_seq(uint32_t ssrc, uint16_t seq);
  int  update_seq(uint32_t ssrc, uint16_t seq);
  void update_lost();

  RtcpBidirectionalStat();
};

#endif
