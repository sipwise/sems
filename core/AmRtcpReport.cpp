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
/** @file AmRtcpReport.cpp - RTCP reports generation and parsing, RFC 3550 */

#include "AmRtcpReport.h"

#include "log.h"

#include <arpa/inet.h>
#include <assert.h>
#include <mutex>
#include <string.h>
#include <strings.h>

#define RTCP_ALIGN_BYTES 4

/** RTCP common header length field: 32-bit words minus one, network order */
#define TO_RTCP_LENGTH(payload_len) htons(((payload_len) >> 2) - 1)

static_assert(sizeof(RtcpSenderReportDataFull) <= RTCP_REPORT_MAX_LEN,
              "RTCP_REPORT_MAX_LEN too small for prepared reports");

static inline void set_rtcp_header(rtcp_hdr_t& h, unsigned char pt, unsigned char c,
                                   unsigned int payload_len)
{
  h.version = RTP_VERSION;
  h.p       = 0;
  h.c       = c;
  h.pt      = pt;
  h.length  = TO_RTCP_LENGTH(payload_len);
}

void RtcpReportsPreparedData::init(unsigned int l_ssrc, const std::string& cname)
{
  // all reports are zeroed, which also provides the terminating
  // SDES END item byte after the CNAME value
  bzero(this, sizeof(RtcpReportsPreparedData));

  // SDES CNAME item, common for all reports
  RtcpSdesData& sdes = rr_empty.sdes;
  sdes.item.type     = RTCP_SDES_CNAME;
  if (!cname.empty() && cname.size() <= INET6_ADDRSTRLEN) {
    sdes.item.len = cname.size();
    memcpy(sdes.data, cname.data(), cname.size());
  }

  // SDES chunk: header + SSRC + item + value + END item, padded to 32-bit border
  unsigned int sdes_len = sizeof(sdes.header) + sizeof(sdes.ssrc) + sizeof(sdes.item) +
                          sdes.item.len + 1;
  sdes_len = (sdes_len + RTCP_ALIGN_BYTES - 1) / RTCP_ALIGN_BYTES * RTCP_ALIGN_BYTES;

  { // empty RR (no report blocks)
    set_rtcp_header(rr_empty.rr.header, RTCP_PT_RR, 0, sizeof(rr_empty.rr));
    rr_empty.rr.sender_ssrc = htonl(l_ssrc);

    rr_empty.sdes = sdes;
    set_rtcp_header(rr_empty.sdes.header, RTCP_PT_SDES, 1, sdes_len);
    rr_empty.sdes.ssrc          = htonl(l_ssrc);
    rr_empty.sdes.packet_length = sdes_len;

    rr_empty.packet_length = sizeof(rr_empty.rr) + sdes_len;
  }

  { // RR with one report block
    set_rtcp_header(rr.rr.header, RTCP_PT_RR, 1, sizeof(rr.rr));
    rr.rr.sender_ssrc = htonl(l_ssrc);

    rr.sdes = sdes;
    set_rtcp_header(rr.sdes.header, RTCP_PT_SDES, 1, sdes_len);
    rr.sdes.ssrc          = htonl(l_ssrc);
    rr.sdes.packet_length = sdes_len;

    rr.packet_length = sizeof(rr.rr) + sdes_len;
  }

  { // SR without a report block
    set_rtcp_header(sr_empty.sr.header, RTCP_PT_SR, 0, sizeof(sr_empty.sr));
    sr_empty.sr.sender.ssrc = htonl(l_ssrc);

    sr_empty.sdes = sdes;
    set_rtcp_header(sr_empty.sdes.header, RTCP_PT_SDES, 1, sdes_len);
    sr_empty.sdes.ssrc          = htonl(l_ssrc);
    sr_empty.sdes.packet_length = sdes_len;

    sr_empty.packet_length = sizeof(sr_empty.sr) + sdes_len;
  }

  { // SR with a report block
    set_rtcp_header(sr.sr.header, RTCP_PT_SR, 1, sizeof(sr.sr));
    sr.sr.sender.ssrc = htonl(l_ssrc);

    sr.sdes = sdes;
    set_rtcp_header(sr.sdes.header, RTCP_PT_SDES, 1, sdes_len);
    sr.sdes.ssrc          = htonl(l_ssrc);
    sr.sdes.packet_length = sdes_len;

    sr.packet_length = sizeof(sr.sr) + sdes_len;
  }
}

void RtcpReportsPreparedData::update(unsigned int r_ssrc)
{
  rr.rr.receiver.ssrc = htonl(r_ssrc);
  sr.sr.receiver.ssrc = rr.rr.receiver.ssrc;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// RTCP compound packet parsing

static int process_sender_report(const RtcpSenderReportHeader& sr, const struct timeval& recv_time,
                                 RtcpBidirectionalStat& stats)
{
  std::lock_guard<AmMutex> l(stats);

  stats.rtcp_sr_recv++;

  DBG("RTCP SR ntp_sec: %u, ntp_frac: %u, rtp_ts: %u, sender_pcount: %u, sender_bcount: %u",
           ntohl(sr.ntp_sec), ntohl(sr.ntp_frac), ntohl(sr.rtp_ts), ntohl(sr.sender_pcount),
           ntohl(sr.sender_bcount));

  // LSR: middle 32 bits of the NTP timestamp of this SR
  stats.sr_lsr = (ntohl(sr.ntp_sec) << 16) | (ntohl(sr.ntp_frac) >> 16);

  stats.sr_recv_time = recv_time;

  return 0;
}

static int process_receiver_report(const RtcpReceiverReportHeader& rr,
                                   const struct timeval& recv_time,
                                   RtcpBidirectionalStat& stats)
{
  std::lock_guard<AmMutex> l(stats);

  stats.rtcp_rr_recv++;

  DBG("RTCP RR ssrc: 0x%x, last_seq: %u, lsr: %u, dlsr: %u, jitter: %u, fract_lost: %u, "
           "total_lost: %u %u %u",
           ntohl(rr.ssrc), ntohl(rr.last_seq), ntohl(rr.lsr), ntohl(rr.dlsr), ntohl(rr.jitter),
           rr.fract_lost, rr.total_lost_0, rr.total_lost_1, rr.total_lost_2);

  if (rr.dlsr) {
    // RTT = now - LSR - DLSR, RFC 3550, sections 4 and 6.4.1;
    // LSR/DLSR are in units of 1/65536 seconds, convert to usec
    int64_t rtt = ((recv_time.tv_sec + NTP_TIME_OFFSET) & 0xffff) * 1000000LL + recv_time.tv_usec;
    rtt -= (uint64_t)ntohl(rr.dlsr) * 1000000LL >> 16;
    rtt -= (uint64_t)ntohl(rr.lsr) * 1000000LL >> 16;

    if (rtt > 0)
      stats.rtt.update(rtt);
  }

  // packet loss of our outbound stream, as reported by the remote side
  stats.tx.loss = (rr.total_lost_2 << 16) | (rr.total_lost_1 << 8) | rr.total_lost_0;

  if (rr.jitter)
    stats.rtcp_remote_jitter.update(ntohl(rr.jitter));

  return 0;
}

static int parse_receiver_reports(unsigned char* chunk, size_t chunk_size,
                                  const struct timeval& recv_time, RtcpBidirectionalStat& stats)
{
  unsigned char* end = chunk + chunk_size;
  do {
    process_receiver_report(*(RtcpReceiverReportHeader*)chunk, recv_time, stats);
    chunk += sizeof(RtcpReceiverReportHeader);
  } while (chunk < end);

  if (chunk != end)
    DBG("received RTCP reports possibly contain garbage");

  return 0;
}

static int parse_sdes(unsigned char* chunk, unsigned char* chunk_end, uint32_t ssrc)
{
  uint8_t sdes_type;
  uint8_t sdes_len;

  bool prev_item_is_null = false;

  while (chunk < chunk_end) {
    sdes_type = *chunk++;

    if (chunk == chunk_end)
      break;

    if (sdes_type == RTCP_SDES_END) {
      prev_item_is_null = true;
      continue;
    }

    if (prev_item_is_null) {
      // start of a new SDES chunk for another SSRC
      prev_item_is_null = false;

      if (chunk + sizeof(uint32_t) > chunk_end)
        break;

      ssrc = *(uint32_t*)chunk;
      chunk += sizeof(uint32_t);

      continue;
    }

    sdes_len = *chunk++;

    if (chunk + sdes_len > chunk_end)
      break;

    DBG("RTCP: SDES item %d with value '%.*s' for SSRC 0x%x", sdes_type, sdes_len, chunk,
             ntohl(ssrc));

    chunk += sdes_len;
  }

  return 0;
}

int rtcp_parse_update_stats(unsigned char* buffer, size_t len, const struct timeval& recv_time,
                            RtcpBidirectionalStat& stats)
{
  unsigned char *r, *end, *chunk_end, *p;
  size_t         chunk_size;
  int            idx;

  assert(buffer);
  assert(len);

  r   = buffer;
  end = r + len;

  idx = 0;
  do {
    chunk_size = end - r;
    if (chunk_size < sizeof(rtcp_hdr_t)) {
      DBG("received RTCP packet part %d is too short: %lu (expected %lu)", idx, chunk_size,
               sizeof(rtcp_hdr_t));
      return -1;
    }

    const rtcp_hdr_t& h = *(rtcp_hdr_t*)r;

    if (h.version != RTP_VERSION) {
      DBG("received RTCP packet with wrong version %u", h.version);
      return -1;
    }

    if (h.p != 0) {
      DBG("received RTCP packet with non-zero padding bit");
      return -1;
    }

    chunk_end = r + sizeof(uint32_t) * (ntohs(h.length) + 1);

    if (chunk_end > end) {
      DBG("RTCP%d: too small buffer for provided chunk length value: %d. "
               "expected at least %lu but tail is %lu",
               idx, ntohs(h.length), chunk_end - r, chunk_size);
      return -1;
    }

    DBG("RTCP chunk %d > version: %u, pt: %u, p: %u, count: %u, length: %u(%lu)", idx,
             h.version, h.pt, h.p, h.c, ntohs(h.length), chunk_end - r);

    switch (h.pt) {
    case RTCP_PT_SR:
      DBG("RTCP: parse Sender Report");
      if (chunk_size < (sizeof(rtcp_hdr_t) + sizeof(RtcpSenderReportHeader) +
                        h.c * sizeof(RtcpReceiverReportHeader))) {
        DBG("RTCP: chunk is too small (%lu) to be a valid SenderReport", chunk_size);
        return -1;
      }

      p = r + sizeof(rtcp_hdr_t);
      process_sender_report(*(RtcpSenderReportHeader*)p, recv_time, stats);

      if (h.c) {
        p += sizeof(RtcpSenderReportHeader);
        parse_receiver_reports(p, chunk_end - p, recv_time, stats);
      } else {
        DBG("SR with empty RR");
      }

      break;

    case RTCP_PT_RR:
      DBG("RTCP: parse Receiver Report");
      if (chunk_size < (sizeof(rtcp_hdr_t) + sizeof(uint32_t) +
                        h.c * sizeof(RtcpReceiverReportHeader))) {
        DBG("RTCP: chunk is too small (%lu) to be a valid ReceiverReport. RC = %u",
                 chunk_size, h.c);
        return -1;
      }

      if (h.c) {
        // skip the SSRC of the RR sender, report blocks follow it
        p = r + sizeof(rtcp_hdr_t) + sizeof(uint32_t);
        parse_receiver_reports(p, chunk_end - p, recv_time, stats);
      } else {
        DBG("got empty RR");
      }

      break;

    case RTCP_PT_SDES: {
      DBG("RTCP: parse Source Description");
      if (chunk_size < (sizeof(rtcp_hdr_t) + sizeof(uint32_t))) {
        DBG("RTCP: chunk is too small (%lu) to be a valid SDES", chunk_size);
        return -1;
      }

      // SSRC of the first SDES chunk follows the common header
      uint32_t ssrc = *(uint32_t*)(r + sizeof(rtcp_hdr_t));
      p             = r + sizeof(rtcp_hdr_t) + sizeof(uint32_t);

      if (parse_sdes(p, chunk_end, ssrc)) {
        DBG("RTCP: failed to parse SDES packet");
        return -1;
      }

      break;
    }

    default: DBG("RTCP: skip parsing unsupported payload type: %d", h.pt);
    } // switch(h.pt)

    r = chunk_end;
    idx++;

  } while (r < end);

  if (r != end)
    DBG("wrong format of the RTCP compound packet");

  return 0;
}
