/*
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the sems software under conditions
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

#ifndef RTP_H
#define RTP_H

/*
 * rtp.h  --  RTP header file (RFC 1889)
 */
#include <sys/types.h>

typedef u_int8_t  u_int8;
typedef unsigned short u_int16;
typedef unsigned int   u_int32;

/*
 * Current protocol version.
 */
#define RTP_VERSION    2

/**
 * \brief RTP data header type
 */
typedef struct {

#if (defined(__BYTE_ORDER) && (__BYTE_ORDER == __BIG_ENDIAN))
    u_int16 version:2;   /* protocol version */
    u_int16 p:1;         /* padding flag */
    u_int16 x:1;         /* header extension flag */
    u_int16 cc:4;        /* CSRC count */
    u_int16 m:1;         /* marker bit */
    u_int16 pt:7;        /* payload type */
#else
    u_int16 cc:4;        /* CSRC count */
    u_int16 x:1;         /* header extension flag */
    u_int16 p:1;         /* padding flag */
    u_int16 version:2;   /* protocol version */
    u_int16 pt:7;        /* payload type */
    u_int16 m:1;         /* marker bit */
#endif
    u_int16 seq;         /* sequence number */
    u_int32 ts;          /* timestamp */
    u_int32 ssrc;        /* synchronization source */
} rtp_hdr_t;

/** \brief RTP extension header type */
typedef struct {
    u_int16 profile;     /* xhdr type */
    u_int16 len;         /* xhdr length */
} rtp_xhdr_t;

/**
 * \brief RTCP data header type
 */
typedef struct
{
#if (defined(__BYTE_ORDER) && (__BYTE_ORDER == __BIG_ENDIAN))
  u_int8 version:2;     /* protocol version */
  u_int8 p:1;           /* padding flag */
  u_int8 c:5;           /* counter */
#else
  u_int8 c:5;           /* counter */
  u_int8 p:1;           /* padding flag */
  u_int8 version:2;     /* protocol version */
#endif

  u_int8 pt;            /* packet type */
  u_int16 length;       /* length */
} rtcp_hdr_t;

typedef struct
{
  u_int32 value;
} rtp_ssrc_t;

#endif
