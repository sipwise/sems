/*
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
/** @file AmSdpBandwidth.cpp - SDP bandwidth modifiers (b=AS/RS/RR) */

#include <cstdlib>
#include <cctype>
#include <cstring>

#include "AmSdpBandwidth.h"
#include "AmConfig.h"

static inline int ceil_div(int a, int b) { return (a + b - 1) / b; }

/** speech bits per channel mode, see 3GPP TS 26.171 (AMR-NB) /
    TS 26.201 / TS 26.202 (AMR-WB), per 20 ms frame */
static const int amr_nb_bits[8]  = { 95, 103, 118, 134, 148, 159, 204, 244 };
static const int amr_wb_bits[9]  = { 132, 177, 261, 285, 317, 365, 397, 461, 477 };

/**
 * Get a codec parameter from an fmtp string ("key=value" entries separated
 * by ';'). Returns false if the parameter is not present.
 */
static bool get_fmtp_param(const string& fmtp, const char* key, string& val)
{
  size_t klen = strlen(key);
  size_t pos = 0;
  while (pos < fmtp.size()) {
    size_t end = fmtp.find(';', pos);
    if (end == string::npos) end = fmtp.size();
    size_t s = pos;
    while (s < end && isspace(fmtp[s])) s++;
    size_t e = end;
    while (e > s && isspace(fmtp[e-1])) e--;
    if (e > s + klen && fmtp.compare(s, klen, key, klen) == 0
        && fmtp[s+klen] == '=') {
      val = fmtp.substr(s+klen+1, e-s-klen-1);
      return true;
    }
    pos = end + 1;
  }
  return false;
}

/**
 * Highest AMR/AMR-WB channel mode from the "mode-set" fmtp parameter
 * ("0,1,2" or "0-7" forms). Defaults to the highest supported mode.
 */
static int amr_max_mode(const string& fmtp, int n_modes)
{
  string ms;
  if (!get_fmtp_param(fmtp, "mode-set", ms) || ms.empty())
    return n_modes - 1;

  int max_mode = -1;
  size_t pos = 0;
  while (pos < ms.size()) {
    size_t end = ms.find(',', pos);
    if (end == string::npos) end = ms.size();
    int lo = atoi(ms.substr(pos, end-pos).c_str());
    size_t dash = ms.find('-', pos);
    int hi = lo;
    if (dash != string::npos && dash < end)
      hi = atoi(ms.substr(dash+1, end-dash-1).c_str());
    if (lo > max_mode) max_mode = lo;
    if (hi > max_mode) max_mode = hi;
    pos = end + 1;
  }

  if (max_mode < 0 || max_mode >= n_modes)
    return n_modes - 1;
  return max_mode;
}

/**
 * AMR/AMR-WB RTP payload size in bytes for one ptime interval.
 * Bandwidth-efficient framing (default) adds CMR (4) + ToC (6) bits;
 * octet-aligned framing adds a single ToC byte (octets-pc mode is ignored).
 */
static int amr_frame_bytes(const int* bits_table, int n_modes,
                           const string& fmtp)
{
  int bits = bits_table[amr_max_mode(fmtp, n_modes)];

  string oa;
  if (get_fmtp_param(fmtp, "octet-align", oa) && atoi(oa.c_str()) == 1)
    return ceil_div(bits, 8) + 1;

  return ceil_div(bits + 10, 8);
}

/**
 * RTP payload (no RTP/UDP/IP headers) size in bytes for one ptime interval.
 * @return bytes or -1 for codecs not covered by the table
 */
static int codec_payload_bytes(const SdpPayload& p, int ptime)
{
  string name = p.encoding_name;
  for (size_t i = 0; i < name.size(); i++)
    name[i] = toupper(name[i]);

  if (name == "PCMU" || name == "PCMA" || name == "G722")
    return 8 * ptime;                       // 64 kbit/s

  if (name == "L16" && p.clock_rate > 0 && (p.clock_rate % 1000) == 0)
    return p.clock_rate / 1000 * 2 * ptime; // 16 bit/sample, mono

  if (name == "GSM")
    return 33 * ceil_div(ptime, 20);        // 33 B / 20 ms frame

  if (name == "ILBC") {
    string mode_s;
    int mode = (get_fmtp_param(p.sdp_format_parameters, "mode", mode_s)
                && atoi(mode_s.c_str()) == 30) ? 30 : 20;
    return (mode == 30 ? 50 : 38) * ceil_div(ptime, mode);
  }

  if (name == "AMR")
    return amr_frame_bytes(amr_nb_bits, 8, p.sdp_format_parameters);

  if (name == "AMR-WB")
    return amr_frame_bytes(amr_wb_bits, 9, p.sdp_format_parameters);

  if (name == "EVS" && p.clock_rate == 16000) {
    string br_s;
    int bitrate = 16400;
    if (get_fmtp_param(p.sdp_format_parameters, "max-bitrate", br_s)) {
      int v = atoi(br_s.c_str());
      if (v > 0) bitrate = v;
    }
    // 10 ms frames: bitrate/100 bits per frame, padded to a byte boundary
    // + 1 class byte, see 3GPP TS 26.445/26.448
    return ceil_div(ptime, 10) * (ceil_div(bitrate, 800) + 1);
  }

  return -1;
}

int AmSdpBandwidth::computeAsKbps(const SdpMedia& m)
{
  const SdpPayload* sel = NULL;
  for (std::vector<SdpPayload>::const_iterator it = m.payloads.begin();
       it != m.payloads.end(); ++it) {
    if (it->encoding_name == "telephone-event")
      continue;
    sel = &*it;
    break;
  }
  if (sel == NULL || sel->encoding_name.empty())
    return -1;

  int ptime = 20;
  for (std::vector<SdpAttribute>::const_iterator it = m.attributes.begin();
       it != m.attributes.end(); ++it) {
    if (it->attribute == "ptime") {
      int v = atoi(it->value.c_str());
      if (v > 0) ptime = v;
      break;
    }
  }

  int payload_bytes = codec_payload_bytes(*sel, ptime);
  if (payload_bytes < 0)
    return -1;

  // IP (20) + UDP (8) + RTP (12); RTCP excluded (TS 26.114 6.2.7.2)
  int total_bytes = payload_bytes + 40;

  // kbit/s = bytes * 8 / ptime, rounded up
  return ceil_div(total_bytes * 8, ptime);
}

void AmSdpBandwidth::apply(SdpMedia& m, const SdpBandwidthCtl& ctl,
                           const SdpMedia* offer)
{
  m.bandwidth.clear();

  if (m.type != MT_AUDIO)
    return;

  // b=AS describes the maximum bandwidth for the receiving direction
  // (3GPP TS 26.114 6.2.7.2): do not emit for sendonly/inactive media.
  if (!m.recv)
    return;

  SdpBandwidthCtl::Mode mode = ctl.mode;
  if (mode == SdpBandwidthCtl::Inherit)
    mode = (AmConfig::SdpBandwidth == AmConfig::SdpBwAuto)
      ? SdpBandwidthCtl::Auto : SdpBandwidthCtl::None;

  if (mode == SdpBandwidthCtl::None)
    return;

  if (mode == SdpBandwidthCtl::Custom) {
    if (ctl.as >= 0) m.bandwidth.push_back("AS:" + std::to_string(ctl.as));
    if (ctl.rr >= 0) m.bandwidth.push_back("RR:" + std::to_string(ctl.rr));
    if (ctl.rs >= 0) m.bandwidth.push_back("RS:" + std::to_string(ctl.rs));
    return;
  }

  // Auto: in an answer echo the offer's b= lines as-is (no recomputation
  // on codec downgrade); in a locally generated offer compute AS+RS+RR.
  if (offer != NULL && !offer->bandwidth.empty()) {
    m.bandwidth = offer->bandwidth;
    return;
  }

  int as = computeAsKbps(m);
  if (as < 0)
    return; // no value is invented for codecs not covered by the table

  m.bandwidth.push_back("AS:" + std::to_string(as));
  int rr = computeRr(as);
  int rs = computeRs(as);
  if (rr > 0) m.bandwidth.push_back("RR:" + std::to_string(rr));
  if (rs > 0) m.bandwidth.push_back("RS:" + std::to_string(rs));
}
