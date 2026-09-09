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
/** @file AmSdpBandwidth.h - SDP bandwidth modifiers (b=AS/RS/RR) */

#ifndef __AmSdpBandwidth_h__
#define __AmSdpBandwidth_h__

#include <vector>
#include <string>

#include "AmSdp.h"

/**
 * Per-session control of SDP bandwidth modifiers (b=AS/RS/RR, RFC 4566/3556).
 *
 * Four states:
 *   Inherit - resolved through global AmConfig::SdpBandwidth
 *   None    - never emit b= lines for this session
 *   Auto    - echo b= from the offer if present, otherwise compute
 *             b=AS from codec + ptime and RS/RR from AS
 *   Custom  - explicit values (as in kbps, rr/rs in bps, -1 = unset)
 *
 * Precedence: session control > global config. Applications are expected to
 * set this per session/call.
 */
struct SdpBandwidthCtl {
    enum Mode { Inherit = 0, None, Auto, Custom };

    Mode mode;
    int  as;  // kbps, -1 unset
    int  rr;  // bps,  -1 unset
    int  rs;  // bps,  -1 unset

    SdpBandwidthCtl() : mode(Inherit), as(-1), rr(-1), rs(-1) {}
};

class AmSdpBandwidth
{
public:
    /**
     * Computes b=AS (application maximum bandwidth, kbps, integer) for a
     * media descriptor from its first non-telephony payload (codec name,
     * clock rate and fmtp) and a=ptime (default 20 ms).
     *
     * AS includes IP(20)/UDP(8)/RTP(12) overhead and excludes RTCP
     * (3GPP TS 26.114 6.2.7.2, receiving direction).
     *
     * @return AS in kbps or -1 if the codec is not covered
     *         (no value is invented for unknown codecs).
     */
    static int computeAsKbps(const SdpMedia& m);

    /** RTCP bandwidth from b=AS, RFC 3556 default shares:
        b=RS = 1.25% and b=RR = 3.75% of the session bandwidth, in bps. */
    static int computeRs(int as_kbps) { return as_kbps < 0 ? -1 : as_kbps * 125 / 10; }
    static int computeRr(int as_kbps) { return as_kbps < 0 ? -1 : as_kbps * 375 / 10; }

    /**
     * Applies the bandwidth policy to a media descriptor, replacing all
     * of its b= lines.
     *
     * @param m     media to modify (answer or locally generated offer)
     * @param ctl   per-session control
     * @param offer matching offer media to echo b= from, or nullptr
     *
     * b= lines describe the maximum bandwidth for the receiving direction,
     * so nothing is applied for media we do not receive (sendonly/inactive).
     * Audio media only.
     */
    static void apply(SdpMedia& m, const SdpBandwidthCtl& ctl,
                      const SdpMedia* offer);
};

#endif
