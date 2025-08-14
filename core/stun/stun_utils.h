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

#ifndef __stun_utils_h__
#define __stun_utils_h__

#include "stun.h"

inline BYTE  get1(const BYTE *data,BYTE i) { return data[i]; }
inline DWORD get2(const BYTE *data,BYTE i) { return (DWORD)(data[i+1]) | ((DWORD)(data[i]))<<8; }
inline DWORD get3(const BYTE *data,BYTE i) { return (DWORD)(data[i+2]) | ((DWORD)(data[i+1]))<<8 | ((DWORD)(data[i]))<<16; }
inline DWORD get4(const BYTE *data,BYTE i) { return (DWORD)(data[i+3]) | ((DWORD)(data[i+2]))<<8 | ((DWORD)(data[i+1]))<<16 | ((DWORD)(data[i]))<<24; }
inline DWORD get8(const BYTE *data,BYTE i) { return ((QWORD)get4(data,i))<<32 | get4(data,i+4); }

inline void set1(BYTE *data,BYTE i,BYTE val)
{
  data[i] = val;
}

inline void set2(BYTE *data,BYTE i,DWORD val)
{
	data[i+1] = (BYTE)(val);
	data[i]   = (BYTE)(val>>8);
}
inline void set3(BYTE *data,BYTE i,DWORD val)
{
  data[i+2] = (BYTE)(val);
  data[i+1] = (BYTE)(val>>8);
  data[i]   = (BYTE)(val>>16);
}
inline void set4(BYTE *data,BYTE i,DWORD val)
{
  data[i+3] = (BYTE)(val);
  data[i+2] = (BYTE)(val>>8);
  data[i+1] = (BYTE)(val>>16);
  data[i]   = (BYTE)(val>>24);
}
inline void set8(BYTE *data,BYTE i,QWORD val)
{
  data[i+7] = (BYTE)(val);
  data[i+6] = (BYTE)(val>>8);
  data[i+5] = (BYTE)(val>>16);
  data[i+4] = (BYTE)(val>>24);
  data[i+3] = (BYTE)(val>>32);
  data[i+2] = (BYTE)(val>>40);
  data[i+1] = (BYTE)(val>>48);
  data[i]   = (BYTE)(val>>56);
}

inline DWORD pad32(DWORD size)
{
  //Alling to 32 bits (8 byte)
  // If it is not a modulus of 4, it needs alignment
  if (size & 0x03)
    //Add padding
    return  (size & 0xFFFFFFFC)+4;
  // Otherwise it is already aligned
  else
    return size;
}

#endif
