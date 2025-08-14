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

#ifndef __crc32calc_h__
#define __crc32calc_h__

static DWORD table[256];
static bool inited;

class CRC32Calc
{
public:
  CRC32Calc()
  {
    //Check if inited globally
    if (!inited)
    {
      for (DWORD i = 0; i < 256; ++i) {
        DWORD c = i;
        for (DWORD j = 0; j < 8; ++j) {
          if (c & 1) {
            c = 0xEDB88320 ^ (c >> 1);
          } else {
            c >>= 1;
          }
        }
        table[i] = c;
      }
      //Initied
      inited = true;
    }

    crc = 0;
  }

  DWORD Update(BYTE *data, DWORD size)
  {
    DWORD c = crc ^ 0xFFFFFFFF;
    for (DWORD i = 0; i < size; ++i)
      c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    crc =  c ^ 0xFFFFFFFF;
    return crc;
  }
private:
  DWORD crc;
};


#endif
