/*
 * $Id: msg_hdrs.h 1713 2010-03-30 14:11:14Z rco $
 *
 * Copyright (C) 2007 Raphael Coeffic
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

#ifndef _msg_hdrs_h
#define _msg_hdrs_h

#include "parse_header.h"
#include "parse_common.h"
#include "parse_via.h"

#include <string>
using std::string;

inline void copy_hdr_wr(string& c, const sip_header* hdr)
{
    c += c2stlstr(hdr->name);

    c += ':';
    c += SP;

    c += c2stlstr(hdr->value);

    c += CR;
    c += LF;
}

inline void contact_wr(string& c,const cstring& contact)
{
    c += "Contact: ";

    c += c2stlstr(contact);

    c += CR;
    c += LF;
}

inline void via_wr(string& c, const cstring& trsp, const cstring& addr, 
		   const cstring& branch, bool rport)
{
    c += "Via: SIP/2.0/";

    for(unsigned int i=0; i<trsp.len; i++) {
      if(trsp.s[i] >= 'a' && trsp.s[i] <= 'z')
	c += trsp.s[i] - 'a' + 'A';
    }

    c += SP;

    c += c2stlstr(addr);

    c += ";branch=" MAGIC_BRANCH_COOKIE;

    c += c2stlstr(branch);

    if(rport){
      c += ";rport";
    }

    c += CR;
    c += LF;
}

inline void cseq_wr(string& c, const cstring& num, const cstring& method)
{
    c += "CSeq: ";

    c += c2stlstr(num);

    c += SP;

    c += c2stlstr(method);

    c += CR;
    c += LF;
}

inline void content_length_wr(string& c, const cstring& len)
{
    c += "Content-Length: ";

    c += c2stlstr(len);

    c += CR;
    c += LF;
}

inline void content_type_wr(string& c, const cstring& len)
{
    c += "Content-Type: ";

    c += c2stlstr(len);

    c += CR;
    c += LF;
}

#include <list>
using std::list;


void copy_hdrs_wr(string& c, const list<sip_header*>& hdrs);
void copy_hdrs_wr_no_via(string& c, const list<sip_header*>& hdrs);
void copy_hdrs_wr_no_via_contact(string& c, const list<sip_header*>& hdrs);


#endif
