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
#include "AmConfigReader.h"
#include "AmConfig.h"
#include "log.h"
#include "md5.h"
#include "AmUtils.h"
#include <string>
#include <fstream>
#include <sstream>

static const std::string CONFIG_FILE_SUFFIX = ".conf";

int AmConfigReader::loadFile(const std::string & path)
{
    std::ifstream file(path);

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream iss(line);
        std::string key;

        if (std::getline(iss, key, '='))
        {
            if (key.front() == '#')
            {
                continue;
            }

            // if = rounded by spaces
            if (key.back() == ' ')
            {
                key.pop_back();
            }

            std::string value;
            if (std::getline(iss, value))
            {
                // if = rounded by spaces
                if (value.front() == ' ')
                {
                    value.erase(0, 1);
                }

                // multiline parameters
                while (value.back() == '\\')
                {
                    value.pop_back();

                    std::string next;
                    if (std::getline(file, next))
                    {
                        while (next.front() == ' ' || next.front() == '\t')
                        {
                            next.erase(0, 1);
                        }

                        value += next;
                    }
                }

                // quoted parameters
                if (value[0] == '"')
                {
                    value.erase(0, 1);
                    value.pop_back();
                }

                config[std::move(key)] = std::move(value);
            }
        }
    }

    return 0;
}

int  AmConfigReader::loadPluginConf(const std::string & path, const std::string & mod_name)
{
    return loadFile(path + mod_name + CONFIG_FILE_SUFFIX);
}

void AmConfigReader::setParameter(const std::string & name, const std::string & val)
{
    config[name] = val;
}

void AmConfigReader::eraseParameter(const std::string & name)
{
    config.erase(name);
}

bool AmConfigReader::hasParameter(const std::string & name) const
{
    return (config.count(name) != 0);
}

const std::string & AmConfigReader::getParameter(const std::string & name) const
{
    static std::string emptyString ("");
    return getParameter(name, emptyString );
}

const std::string & AmConfigReader::getParameter(const std::string & name, const std::string & defval) const
{
    std::map<std::string, std::string>::const_iterator it = config.find(name);

    return (it == config.end() ? defval : it->second);
}

int AmConfigReader::getParameterInt(const std::string & name, int defval) const
{
    int result=0;

    if (!hasParameter(name) || !str2int(getParameter(name),result))
    {
        return defval;
    }
    else
    {
        return result;
    }
}

bool AmConfigReader::getMD5(const string& path, string& md5hash, bool lowercase)
{
    std::ifstream data_file(path.c_str(), std::ios::in | std::ios::binary);

    if (!data_file)
    {
        DBG("could not read file '%s'\n", path.c_str());
        return false;
    }

    // that one is clever...
    // (see http://www.gamedev.net/community/forums/topic.asp?topic_id=353162 )
    std::string file_data((std::istreambuf_iterator<char>(data_file)), std::istreambuf_iterator<char>());

    if (file_data.empty())
    {
        return false;
    }

    MD5_CTX md5ctx;
    MD5Init(&md5ctx);
    MD5Update(&md5ctx, (unsigned char*)file_data.c_str(), file_data.length());
    unsigned char _md5hash[16];
    MD5Final(_md5hash, &md5ctx);
    md5hash = "";

    for (size_t i=0;i<16;i++)
    {
        md5hash += char2hex(_md5hash[i], lowercase);
    }

    return true;
}

void AmConfigReader::dump() const
{
    for(map<string,string>::const_iterator it = config.begin(); it != config.end(); it++)
    {
        DBG("\t%s = %s",it->first.c_str(),it->second.c_str());
    }
}
