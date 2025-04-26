/*
 * Copyright (C) 2006 Stefan Sayer
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

#include "AmUAC.h"
#include "AmSipMsg.h"
#include "AmSession.h"
#include "AmSessionContainer.h"
#include "AmConfig.h"

string AmUAC::dialout(const string& user,
		      const string& app_name,
		      const string& r_uri, 
		      const string& from,
		      const string& from_uri,
		      const string& to,
		      const string& local_tag,
		      const string& hdrs,
		      const AmArg*  session_params) {
 
  AmSipRequest req;
  string m_app_name = app_name;

  req.user     = user;
  req.method   = "INVITE";
  req.r_uri    = r_uri;
  req.from     = from;
  req.from_uri = from_uri;
  if (!local_tag.length())
    req.from_tag   = AmSession::getNewId();
  else 
    req.from_tag   = local_tag;
  req.to       = to;
  req.to_tag   = "";
  req.callid   = AmSession::getNewId();
  req.hdrs     = hdrs;
    
  return AmSessionContainer::instance()->startSessionUAC(req, m_app_name, session_params);
}

