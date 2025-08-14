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

#ifndef __AmStunPacket_h__
#define __AmStunPacket_h__

#include "stun/stun_utils.h"

#include <vector>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

class AmStunPacket
{
public:
  enum Type  {Request=0,Indication=1,Response=2,Error=3};
  enum Method {Binding=1};

  struct Attribute
  {
    enum Type {
      MappedAddress = 0x0001,
      Username = 0x0006,
      MessageIntegrity = 0x0008,
      ErrorCode = 0x0009,
      UnknownAttributes = 0x000A,
      Realm = 0x0014,
      Nonce = 0x0015,
      Priority = 0x0024,
      UseCandidate = 0x0025,
      XorMappedAddress = 0x0020,
      Software = 0x8022,
      AlternateServer = 0x8023,
      FingerPrint = 0x8028,
      IceControlled = 0x8029,
      IceControlling = 0x802A
    };

    Attribute(WORD type,BYTE *attr,WORD size)
    {
      //Copy values
      this->type = type;
      this->size = size;
      if (attr)
      {
        //allocate
        this->attr = (BYTE*)malloc(size);
        //Copy
        memcpy(this->attr,attr,size);
      } else {
        //Empty
        this->attr = NULL;
      }
    }

    Attribute(WORD type,QWORD data)
    {
      //Copy values
      this->type = type;
      this->size = sizeof(data);
      //allocate
      this->attr = (BYTE*)malloc(size);
      //Copy
      set8(this->attr,0,data);
    }

    Attribute(WORD type,DWORD data)
    {
      //Copy values
      this->type = type;
      this->size = sizeof(data);
      //allocate
      this->attr = (BYTE*)malloc(size);
      //Copy
      set4(this->attr,0,data);
    }

    ~Attribute()
    {
      if (attr)
        free(attr);
    }

    Attribute *Clone()
    {
      return new Attribute(type,attr,size);
    }
    WORD type;
    WORD size;
    BYTE *attr;
  };
public:
  AmStunPacket(Type type,Method method,BYTE* transId);
  ~AmStunPacket();
  AmStunPacket* createResponse();

  /** Check STUN packet **/
  static bool isPacketStun(unsigned char *buffer, size_t len);

  /**
   * Parse the given data into an AmStunPacket.
   * @return a new stun packet or NULL on error
   */
  static AmStunPacket* parse(BYTE* data,DWORD size);

  /**
   * Serialize the stun packet into the given data
   * and authenticate it with the given password.
   * @return the size of the serialized data
   */
  DWORD authenticatedFingerPrint(BYTE* data,DWORD size,const char* pwd);

  /**
   * Serialize the stun packet into the given data
   * without authenticating.
   * @return the size of the serialized data
   */
  DWORD nonAuthenticatedFingerPrint(BYTE* data,DWORD size);

  /** get the packet size */
  DWORD getSize();

  /**
   * get the value of the given attribute type.
   * @return Attribute if exists or null otherwise
   */
  Attribute* getAttribute(Attribute::Type type);

  /** check the existence of an attribute of the given type */
  bool  hasAttribute(Attribute::Type type);

  /** add the given attribute */
  void  addAttribute(Attribute* attr);

  /**
   * add an attribute given the type, the data
   * containing the value and the size of the value.
   */
  void  addAttribute(Attribute::Type type,BYTE *data,DWORD size);

  /**
   * add an attribute given the type and the data
   * containing the value.
   */
  void  addAttribute(Attribute::Type type,QWORD data);
  void  addAttribute(Attribute::Type type,DWORD data);

  /** add an empty attribute of the given type */
  void  addAttribute(Attribute::Type type);

  void  addAddressAttribute(sockaddr_storage *addr);
  void  addXorAddressAttribute(sockaddr_storage *addr);
  void  addUsernameAttribute(const char* local,const char* remote);

  Type getType()		{ return type; }
  Method getMethod()	{ return method; }
private:
  typedef std::vector<Attribute*> Attributes;
private:
  Type	type;
  Method	method;
  BYTE	transId[12];
  Attributes attributes;
};

#endif
