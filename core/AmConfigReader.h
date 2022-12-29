/*
 * Copyright (C) 2002-2003 Fhg Fokus
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
/** @file AmConfigReader.h */
#ifndef AmConfigReader_h
#define AmConfigReader_h

#include <string>
#include <map>
using std::string;


#define MAX_CONFIG_LINE 4096
#define CONFIG_FILE_SUFFIX ".conf"

/**
 * \brief configuration file reader
 * 
 * Reads configuration file into internal map, 
 * which subsequently can be queried for the value of
 * specific configuration values.
 */

class AmConfigReader
{
  std::map<string,string> keys;

 public:
  int  loadFile(const string& path);
  int  loadPluginConf(const string& mod_name);
  int  loadString(const char* cfg_lines, size_t cfg_len);

  /** get md5 hash of file contents */
  bool getMD5(const string& path, string& md5hash, bool lowercase = true);
  void setParameter(const string& param, const string& val);
  void eraseParameter(const string& param);
  bool hasParameter(const string& param) const;

  const string& getParameter(const string& param) const;
  const string& getParameter(const string& param, const string& defval) const;
  unsigned int getParameterInt(const string& param, unsigned int defval = 0) const;

  std::map<string,string>::const_iterator begin() const
    { return keys.begin(); }

  std::map<string,string>::const_iterator end() const
    { return keys.end(); }

  void dump();
};

#endif
