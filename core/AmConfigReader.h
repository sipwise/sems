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
#pragma once
#include <string>
#include <map>

/**
 * \brief configuration file reader
 * 
 * Reads configuration file into internal map, 
 * which subsequently can be queried for the value of
 * specific configuration values.
 */

class AmConfigReader
{
    std::map<std::string, std::string> config;

public:
    int  loadFile(const std::string & path);
    int  loadPluginConf(const std::string & path, const std::string & mod_name);

    void setParameter(const std::string & name, const std::string & val);
    void eraseParameter(const std::string & name);
    bool hasParameter(const std::string & name) const;

    const std::string & getParameter(const std::string & name) const;
    const std::string & getParameter(const std::string & name, const std::string & defval) const;
    int getParameterInt(const std::string & name, int defval = 0) const;

    std::map<std::string, std::string>::const_iterator begin() { return config.begin(); }
    std::map<std::string, std::string>::const_iterator end() { return config.end(); }

    bool getMD5(const std::string & path, std::string & md5hash, bool lowercase = true);
    void dump() const;
};
