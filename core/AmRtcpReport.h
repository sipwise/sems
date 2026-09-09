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
/** @file AmRtcpReport.h - RTCP packet format, reports generation and parsing,
 *    RFC 3550 */

#ifndef __AmRtcpReport_h__
#define __AmRtcpReport_h__

#include "AmRtcpStat.h"
#include "rtp/rtp.h"

#include <netinet/in.h>
#include <sys/time.h>

#include <string>

// difference between NTP and POSIX epochs (1900 vs 1970), seconds
#define NTP_TIME_OFFSET 2208988800ULL

/** RTCP payload types, RFC 3550, section 3 (the common packet
 *  header itself is rtcp_hdr_t, see rtp/rtp.h) */
enum rtcp_payload_type {
  RTCP_PT_SR   = 200,
  RTCP_PT_RR   = 201,
  RTCP_PT_SDES = 202,
  RTCP_PT_BYE  = 203,
  RTCP_PT_APP  = 204
};

/** RTCP SDES item types, RFC 3550, section 6.5 */
enum rtcp_sdes_item_type {
  RTCP_SDES_END   = 0,
  RTCP_SDES_CNAME = 1,
  RTCP_SDES_NAME  = 2,
  RTCP_SDES_EMAIL = 3,
  RTCP_SDES_PHONE = 4,
  RTCP_SDES_LOC   = 5,
  RTCP_SDES_TOOL  = 6,
  RTCP_SDES_NOTE  = 7
};

/** RTCP SR packet fields following the common header, RFC 3550, section 4 */
struct RtcpSenderReportHeader {
  uint32_t ssrc;          /**< SSRC of packet sender        */
  uint32_t ntp_sec;       /**< NTP time, seconds part.      */
  uint32_t ntp_frac;      /**< NTP time, fractions part.    */
  uint32_t rtp_ts;        /**< RTP timestamp.               */
  uint32_t sender_pcount; /**< Sender packet count.         */
  uint32_t sender_bcount; /**< Sender octet/bytes count.    */
};

/** RTCP RR report block, RFC 3550, section 4 */
struct RtcpReceiverReportHeader {
  uint32_t ssrc; /**< SSRC of the source being reported.   */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  uint32_t fract_lost   : 8; /**< Fraction lost.         */
  uint32_t total_lost_2 : 8; /**< Total lost, bit 16-23. */
  uint32_t total_lost_1 : 8; /**< Total lost, bit 8-15.  */
  uint32_t total_lost_0 : 8; /**< Total lost, bit 0-7.   */
#else
  uint32_t fract_lost   : 8; /**< Fraction lost.         */
  uint32_t total_lost_2 : 8; /**< Total lost, bit 0-7.   */
  uint32_t total_lost_1 : 8; /**< Total lost, bit 8-15.  */
  uint32_t total_lost_0 : 8; /**< Total lost, bit 16-23. */
#endif
  uint32_t last_seq; /**< Extended last sequence number.  */
  uint32_t jitter;   /**< Inter-arrival jitter.           */
  uint32_t lsr;      /**< Last SR timestamp (LSR).        */
  uint32_t dlsr;     /**< Delay since last SR (DLSR).     */
};

/** RTCP SDES chunk (header + SSRC + single item + value), RFC 3550, section 6.5 */
struct RtcpSdesData {
  rtcp_hdr_t header;
  uint32_t   ssrc;
  struct {
    uint8_t type;
    uint8_t len;
  } item;
  /* CNAME up to INET6_ADDRSTRLEN requires 1 byte padding
   * (4 + 4 + 2 + 46 + 1 END item byte + 1 pad byte),
   * https://www.rfc-editor.org/rfc/rfc3550#section-6.5 */
  unsigned char  data[INET6_ADDRSTRLEN + 1];
  unsigned int   packet_length;
};

/** Prepared compound SR packet with a receiver report block */
struct RtcpSenderReportDataFull {
  struct {
    rtcp_hdr_t               header;
    RtcpSenderReportHeader   sender;
    RtcpReceiverReportHeader receiver;
  } sr;
  RtcpSdesData sdes;
  unsigned int packet_length;
};

/** Prepared compound SR packet without a receiver report block */
struct RtcpSenderReportDataNoReceiver {
  struct {
    rtcp_hdr_t             header;
    RtcpSenderReportHeader sender;
  } sr;
  RtcpSdesData sdes;
  unsigned int packet_length;
};

/** Prepared compound RR packet with a receiver report block */
struct RtcpReceiverReportDataFull {
  struct {
    rtcp_hdr_t               header;
    uint32_t                 sender_ssrc;
    RtcpReceiverReportHeader receiver;
  } rr;
  RtcpSdesData sdes;
  unsigned int packet_length;
};

/** Prepared compound RR packet without report blocks */
struct RtcpEmptyReceiverReport {
  struct {
    rtcp_hdr_t header;
    uint32_t   sender_ssrc;
  } rr;
  RtcpSdesData sdes;
  unsigned int packet_length;
};

/** Max size of a prepared compound report (SR/RR + SDES CNAME), bytes */
const unsigned int RTCP_REPORT_MAX_LEN = 128;

/** Precompiled SR/RR reports of an RTP stream: only changing fields
 *  (timestamps, counters) are filled in before sending */
struct RtcpReportsPreparedData {
  RtcpEmptyReceiverReport        rr_empty; // no report blocks
  RtcpSenderReportDataNoReceiver sr_empty; // no report blocks
  RtcpReceiverReportDataFull     rr;
  RtcpSenderReportDataFull       sr;

  void init(unsigned int l_ssrc, const std::string& cname);
  void update(unsigned int r_ssrc);
};

/**
 * Parse a compound RTCP packet (SR/RR/SDES) and update stream statistics.
 *
 * @param buffer     received RTCP packet
 * @param len        packet length
 * @param recv_time  receive timestamp of the packet (used for LSR/DLSR/RTT)
 * @param stats      per-stream statistics to update
 * @return 0 on success, -1 on parse error
 */
int rtcp_parse_update_stats(unsigned char* buffer, size_t len,
                            const struct timeval& recv_time,
                            RtcpBidirectionalStat& stats);

#endif
