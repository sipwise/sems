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

#include "AmStunPacket.h"
#include "crc32calc.h"

#include <assert.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>

/*
 * The magic cookie value is: 0x2112A442
 * This is such value, chunked in bytes in network byte order.
 */
static const BYTE MagicCookie[4] = {0x21,0x12,0xA4,0x42};

AmStunPacket::AmStunPacket(Type type,Method method,BYTE* transId)
{
  //Store values
  this->type = type;
  this->method = method;
  //Copy
  memcpy(this->transId,transId,12);
}

AmStunPacket::~AmStunPacket()
{
  //For each
  for (Attributes::iterator it = attributes.begin(); it!=attributes.end(); ++it)
    //Delete attr
    delete (*it);
}

AmStunPacket* AmStunPacket::parse(BYTE* data,DWORD size)
{
  //Check size
  if (size < 20)
    //It is not
    return NULL;
  //Check first two bits are cero
  if (data[0] & 0xC0)
    //It is not
    return NULL;
  //Check magic cooke
  if (memcmp(MagicCookie,data+4,4))
    //It is not
    return NULL;
  /*
   * The message type field is decomposed further into the following
    structure:

        0                 1
        2  3  4 5 6 7 8 9 0 1 2 3 4 5
        +--+--+-+-+-+-+-+-+-+-+-+-+-+-+
        |M |M |M|M|M|C|M|M|M|C|M|M|M|M|
        |11|10|9|8|7|1|6|5|4|0|3|2|1|0|
        +--+--+-+-+-+-+-+-+-+-+-+-+-+-+

      Figure 3: Format of STUN Message Type Field

    Here the bits in the message type field are shown as most significant
    (M11) through least significant (M0).  M11 through M0 represent a 12-
    bit encoding of the method.  C1 and C0 represent a 2-bit encoding of
    the class.
  */
  //Get all bytes
  WORD method = get2(data,0);
  //Normalize
  method =  (method & 0x000f) | ((method & 0x00e0)>>1) | ((method & 0x3E00)>>2);
  //Get class
  WORD type = ((data[0] & 0x01) << 1) | ((data[1] & 0x10) >> 4);

  //Create new message
  AmStunPacket* msg = new AmStunPacket((Type)type,(Method)method,data+8);


  unsigned int i = 20;
  /*
    STUN Attributes

    After the STUN header are zero or more attributes.  Each attribute
    MUST be TLV encoded, with a 16-bit type, 16-bit length, and value.
    Each STUN attribute MUST end on a 32-bit boundary.  As mentioned
    above, all fields in an attribute are transmitted most significant
    bit first.
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |         Type                  |            Length             |
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        |                         Value (variable)                ....
        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

  */

  //Read atributes
  while (i+4<size)
  {
    //Get attribute type
    WORD attrType = get2(data,i);
    WORD attrLen = get2(data,i+2);
    //Check size
    if (size<i+4+attrLen)
      //Skip
      break;
    //Add it
    msg->addAttribute((Attribute::Type)attrType,data+i+4,attrLen);

    //Next
    i = pad32(i+4+attrLen);
  }

  //Return it
  return msg;
}
DWORD AmStunPacket::nonAuthenticatedFingerPrint(BYTE* data,DWORD size)
{

  //Get size - Message attribute - FINGERPRINT
  WORD msgSize = getSize()-24-8;

  //Check
  assert(size>=msgSize);

  //Convert so we can sift
  WORD msgType = type;
  WORD msgMethod = method;

  //Merge the type and method
  DWORD msgTypeField =  (msgMethod & 0x0f80) << 2;
  msgTypeField |= (msgMethod & 0x0070) << 1;
  msgTypeField |= (msgMethod & 0x000f);
  msgTypeField |= (msgType & 0x02) << 7;
  msgTypeField |= (msgType & 0x01) << 4;

  //Set it
  set2(data,0,msgTypeField);

  //Set attributte length
  set2(data,2,msgSize-20);

  //Set cookie
  memcpy(data+4,MagicCookie,4);

  //Set trnasaction
  memcpy(data+8,transId,12);

  DWORD i = 20;

  //For each
  for (Attributes::iterator it = attributes.begin(); it!=attributes.end(); ++it)
  {
    //Set attr type
    set2(data,i,(*it)->type);
    set2(data,i+2,(*it)->size);
    //Check not empty attr
    if ((*it)->attr)
      //Copy
      memcpy(data+i+4,(*it)->attr,(*it)->size);
    //Move
    i = pad32(i+4+(*it)->size);
  }

  //Return size
  return i;
}

DWORD AmStunPacket::authenticatedFingerPrint(BYTE* data,DWORD size,const char* pwd)
{
  //Get size
  WORD msgSize = getSize();

  //Check
  assert(size>=msgSize);

  //Convert so we can sift
  WORD msgType = type;
  WORD msgMethod = method;

  //Merge the type and method
  DWORD msgTypeField =  (msgMethod & 0x0f80) << 2;
  msgTypeField |= (msgMethod & 0x0070) << 1;
  msgTypeField |= (msgMethod & 0x000f);
  msgTypeField |= (msgType & 0x02) << 7;
  msgTypeField |= (msgType & 0x01) << 4;

  //Set it
  set2(data,0,msgTypeField);

  //Set attributte length
  set2(data,2,msgSize-20);

  //Set cookie
  memcpy(data+4,MagicCookie,4);

  //Set trnasaction
  memcpy(data+8,transId,12);

  DWORD i = 20;

  //For each
  for (Attributes::iterator it = attributes.begin(); it!=attributes.end(); ++it)
  {
    //Set attr type
    set2(data,i,(*it)->type);
    set2(data,i+2,(*it)->size);
    //Check not empty attr
    if ((*it)->attr)
            //Copy
            memcpy(data+i+4,(*it)->attr,(*it)->size);
    //Move
    i = pad32(i+4+(*it)->size);
  }

  DWORD len;
  CRC32Calc crc32calc;

  //Change length to omit the Fingerprint attribute from the HMAC calculation of the message integrity
  set2(data,2,msgSize-20-8);

  //Calculate HMAC and put it in the attibute value
  HMAC(EVP_sha1(),(BYTE*)pwd, strlen(pwd),data,i,data+i+4,&len);

  //Set message integriti attribute
  set2(data,i,Attribute::MessageIntegrity);
  set2(data,i+2,len);

  //INcrease sixe
  i = pad32(i+4+len);

  //REstore length
  set2(data,2,msgSize-20);

  //Calculate crc 32 XOR'ed with the 32-bit value 0x5354554e
  DWORD crc32 = crc32calc.Update(data,i) ^ 0x5354554e;

  //Set fingerprint attribute
  set2(data,i,Attribute::FingerPrint);
  set2(data,i+2,4);
  set4(data,i+4,crc32);

  //INcrease sixe
  i = pad32(i+8);

  //Return size
  return i;
}

DWORD AmStunPacket::getSize()
{
  //Base message + Message attribute + FINGERPRINT attribute
  DWORD size = 20+24+8;

  //For each
  for (Attributes::iterator it = attributes.begin(); it!=attributes.end(); ++it)
    //Inc size with padding
    size = pad32(size+4+(*it)->size);

  //Write it
  return size;
}

AmStunPacket::Attribute* AmStunPacket::getAttribute(Attribute::Type type)
{
  //For each
  for (Attributes::iterator it = attributes.begin(); it!=attributes.end(); ++it)
    //Check attr
    if ((*it)->type==type)
      //Return it
      return (*it);
  //Not found
  return NULL;
}

bool AmStunPacket::hasAttribute(Attribute::Type type)
{
  //For each
  for (Attributes::iterator it = attributes.begin(); it!=attributes.end(); ++it)
    //Check attr
    if ((*it)->type==type)
      //Return it
      return true;
  //Not found
  return false;
}

void  AmStunPacket::addAttribute(Attribute* attr)
{
  //Add it
  attributes.push_back(attr);
}

void  AmStunPacket::addAttribute(Attribute::Type type,BYTE *data,DWORD size)
{
  //Add it
  attributes.push_back(new Attribute(type,data,size));
}

void  AmStunPacket::addAttribute(Attribute::Type type)
{
  //Add it
  attributes.push_back(new Attribute(type,NULL,0));
}

void  AmStunPacket::addAttribute(Attribute::Type type,QWORD data)
{
  //Add it
  attributes.push_back(new Attribute(type,data));
}

void  AmStunPacket::addAttribute(Attribute::Type type,DWORD data)
{
  //Add it
  attributes.push_back(new Attribute(type,data));
}

void  AmStunPacket::addUsernameAttribute(const char* local,const char* remote)
{
  //Calculate new size
  DWORD size = strlen(local)+strlen(remote)+1;
  //Allocate data
  BYTE* data =(BYTE*)malloc(size+1);
  //Create username
  sprintf((char*)data,"%s:%s",remote,local);
  //Add attribute
  addAttribute(Attribute::Username,data,size);
  //Free mem
  free(data);
}

void  AmStunPacket::addAddressAttribute(sockaddr_storage* addr)
{
  BYTE aux[8];
  sockaddr_in* in_addr = (sockaddr_in*)addr;

  //Unused
  aux[0] = 0;
  //Family
  aux[1] = 1;
  //Set port
  memcpy(aux+2,&in_addr->sin_port,2);
  //Set addres
  memcpy(aux+4,&in_addr->sin_addr.s_addr,4);
  //Add it
  addAttribute(Attribute::MappedAddress,aux,8);
}

void  AmStunPacket::addXorAddressAttribute(sockaddr_storage* addr)
{
  BYTE aux[8];
  sockaddr_in* in_addr = (sockaddr_in*)addr;

  //Unused
  aux[0] = 0;
  //Family
  aux[1] = 1;
  //Set port
  memcpy(aux+2,&in_addr->sin_port,2);
  //Xor it
  aux[2] ^= MagicCookie[0];
  aux[3] ^= MagicCookie[1];
  //Set addres
  memcpy(aux+4,&in_addr->sin_addr.s_addr,4);
  //Xor it
  aux[4] ^= MagicCookie[0];
  aux[5] ^= MagicCookie[1];
  aux[6] ^= MagicCookie[2];
  aux[7] ^= MagicCookie[3];
  //Add it
  addAttribute(Attribute::XorMappedAddress,aux,8);
}

AmStunPacket* AmStunPacket::createResponse()
{
  return new AmStunPacket(Response,method,transId);
}

bool AmStunPacket::isPacketStun(unsigned char *buffer, size_t len) {
  return (
  // STUN headers are 20 bytes.
  (len >= 20) &&
  // First two bits must be zero.
  !(buffer[0] & 0xC0) &&
  // Magic cookie must match.
  (buffer[4] == MagicCookie[0]) &&
  (buffer[5] == MagicCookie[1]) &&
  (buffer[6] == MagicCookie[2]) &&
  (buffer[7] == MagicCookie[3])
  );
}


