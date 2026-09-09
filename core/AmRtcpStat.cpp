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
/** @file AmRtcpStat.cpp - RTCP stream statistics, RFC 3550 */

#include "AmRtcpStat.h"

#define RTP_SEQ_MOD (1 << 16)

RtcpUnidirectionalStat::RtcpUnidirectionalStat()
  : pkt(0)
  , bytes(0)
  , loss(0)
  , reorder(0)
  , dup(0)
  , rtcp_jitter(0)
{
}

RtcpBidirectionalStat::RtcpBidirectionalStat()
  : current_rx(nullptr)
  , relay_tx_pkt(0)
  , relay_tx_bytes(0)
  , rtcp_rr_sent(0)
  , rtcp_rr_recv(0)
  , rtcp_sr_sent(0)
  , rtcp_sr_recv(0)
  , max_seq(0)
  , cycles(0)
  , base_seq(0)
  , bad_seq(0)
  , probation(0)
  , received(0)
  , expected_prior(0)
  , received_prior(0)
  , transit(0)
  , total_lost(0)
  , fraction_lost(0)
  , sr_lsr(0)
{
  timerclear(&rx_recv_time);
  timerclear(&sr_recv_time);
}

void RtcpBidirectionalStat::init_seq(uint32_t ssrc, uint16_t seq)
{
  base_seq       = seq;
  max_seq        = seq;
  bad_seq        = RTP_SEQ_MOD - 1;
  cycles         = 0;
  total_lost     = 0;
  fraction_lost  = 0;
  received       = 0;
  received_prior = 0;
  expected_prior = 0;
  transit        = 0;
  if (rx.size() < MAX_RX_STATS || rx.find(ssrc) != rx.end()) {
    current_rx              = &rx[ssrc];
    current_rx->rtcp_jitter = 0;
  } else {
    // too many accounted sources for this stream,
    // don't track this one (see MAX_RX_STATS)
    current_rx = nullptr;
  }
}

/** RTP sequence number validation and update,
 *  based on rtp_update_sequence_numbers() from pjmedia,
 *  following RFC 3550, appendix A.1.
 *  @return 1 if the packet is in order (or the source was (re)initialized),
 *          0 if it is a duplicate, reordered or unexpected packet */
int RtcpBidirectionalStat::update_seq(uint32_t ssrc, uint16_t seq)
{
  uint16_t udelta = seq - max_seq;

  /* Source is not valid until MIN_SEQUENTIAL packets with
   * sequential sequence numbers have been received. */
  if (probation) {
    // packet is in sequence
    if (seq == max_seq + 1) {
      probation--;
      max_seq = seq;
      if (probation == 0) {
        init_seq(ssrc, seq);
        received++;
        return 1;
      }
    } else {
      probation = MIN_SEQUENTIAL - 1;
      max_seq   = seq;
    }
    return 0;
  } else if (udelta < MAX_DROPOUT) {
    // in order, with permissible gap
    if (seq < max_seq) {
      // Sequence number wrapped - count another 64K cycle.
      cycles += RTP_SEQ_MOD;
    }
    max_seq = seq;
  } else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {
    // the sequence number made a very large jump
    if (seq == bad_seq) {
      /* Two sequential packets -- assume that the other side
       * restarted without telling us so just re-sync
       * (i.e., pretend this was the first packet). */
      init_seq(ssrc, seq);
    } else {
      bad_seq = (seq + 1) & (RTP_SEQ_MOD - 1);
      return 0;
    }
  } else {
    // duplicate or reordered packet
    return 1;
  }

  received++;
  return 1;
}

void RtcpBidirectionalStat::update_lost()
{
  if (!max_seq)
    return;

  uint32_t extended_max = cycles + max_seq;
  uint32_t expected     = extended_max - base_seq + 1;

  total_lost = expected - received;

  uint32_t expected_interval = expected - expected_prior;
  expected_prior             = expected;

  uint32_t received_interval = received - received_prior;
  received_prior             = received;

  uint32_t lost_interval = expected_interval - received_interval;
  if (expected_interval == 0 || lost_interval <= 0) {
    fraction_lost = 0;
  } else {
    fraction_lost = (lost_interval << 8) / expected_interval;
  }
}
