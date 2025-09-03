/*
 * Copyright (C) 2009 Teltech Systems Inc.
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
#include "ModUtils.h"
#include "log.h"
#include "AmUtils.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "DSMSession.h"
#include "AmSession.h"
#include "AmPlaylist.h"

/* voucher */
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>

SC_EXPORT(MOD_CLS_NAME);

MOD_ACTIONEXPORT_BEGIN(MOD_CLS_NAME) {

  DEF_CMD("utils.playCountRight", SCUPlayCountRightAction);
  DEF_CMD("utils.playCountLeft",  SCUPlayCountLeftAction);
  DEF_CMD("utils.getCountRight",  SCUGetCountRightAction);
  DEF_CMD("utils.getCountLeft",   SCUGetCountLeftAction);
  DEF_CMD("utils.getCountRightNoSuffix",  SCUGetCountRightNoSuffixAction);
  DEF_CMD("utils.getCountLeftNoSuffix",   SCUGetCountLeftNoSuffixAction);
  DEF_CMD("utils.getStringToChar",   SCUGetStringToCharAction);

  DEF_CMD("utils.getNewId", SCGetNewIdAction);
  DEF_CMD("utils.spell", SCUSpellAction);
  DEF_CMD("utils.rand", SCURandomAction);
  DEF_CMD("utils.srand", SCUSRandomAction);
  DEF_CMD("utils.add", SCUSAddAction);
  DEF_CMD("utils.sub", SCUSSubAction);
  DEF_CMD("utils.int", SCUIntAction);
  DEF_CMD("utils.splitStringCR", SCUSplitStringAction);
  DEF_CMD("utils.escapeCRLF", SCUEscapeCRLFAction);
  DEF_CMD("utils.unescapeCRLF", SCUUnescapeCRLFAction);
  DEF_CMD("utils.playRingTone", SCUPlayRingToneAction);

  DEF_CMD("utils.encryptCodeAes128CBC", SCEncryptCodeAes128CBC);
  DEF_CMD("utils.decryptCodeAes128CBC", SCDecryptCodeAes128CBC);

} MOD_ACTIONEXPORT_END;

MOD_CONDITIONEXPORT_BEGIN(MOD_CLS_NAME) {
  if (cmd == "utils.isInList") {
    return new IsInListCondition(params, false);
  }

} MOD_CONDITIONEXPORT_END;

/**
 * Helper functions
 */

vector<string> utils_get_count_files(DSMSession* sc_sess, unsigned int cnt, 
				     const string& basedir, const string& suffix, bool right) {
  
  vector<string> res;

  unsigned int number = cnt;
  unsigned int n = log10(number) + 1;
  const int max = 9;

  string full_number = std::to_string(number);
  string remainder; /* what remains after 9 digits, if remains */
  bool remainder_exists = false;

  if (cnt <= 20) {
    res.push_back(basedir+int2str(cnt)+suffix);
    return res;
  }

  if (n > max) {
    remainder_exists = true;
    remainder = full_number.substr(max);
    cnt = std::stoi(remainder);
  }

  for (int i = n; i > 0; i--) {
    int num = (int)((number % (unsigned int)pow(10, i)) / pow(10, i - 1));
    if ( (n - i) < max )
      res.push_back(basedir+int2str(num)+suffix); /* only push until max amount here */
  }

  if (!remainder_exists)
    return res;

  if ((cnt <= 20) || (!(cnt%10))) {
    res.push_back(basedir+int2str(cnt)+suffix);
    return res;
  }

  div_t num = div(cnt, 10);
  if (right) { 
   // language has single digits before 10s
    res.push_back(basedir+int2str(num.quot * 10)+suffix);
    res.push_back(basedir+(int2str(num.rem)+"-and")+suffix);
  } else {
    // language has single digits before 10s
    res.push_back(basedir+(int2str(num.rem)+"-and")+suffix);
    res.push_back(basedir+int2str(num.quot * 10)+suffix);
  }

  return res;
}


bool utils_play_count(DSMSession* sc_sess, unsigned int cnt, 
		      const string& basedir, const string& suffix, bool right) {
  
  if (cnt <= 20) {
    sc_sess->playFile(basedir+int2str(cnt)+suffix, false);
    return false;
  }
  
  for (int i=9;i>1;i--) {
    div_t num = div(cnt, (int)pow(10.,i));  
    if (num.quot) {
      sc_sess->playFile(basedir+int2str(int(num.quot * pow(10.,i)))+suffix, false);
    }
    cnt = num.rem;
  }

  if (!cnt)
    return false;

  if ((cnt <= 20) || (!(cnt%10))) {
    sc_sess->playFile(basedir+int2str(cnt)+suffix, false);
    return false;
  }

  div_t num = div(cnt, 10);
  if (right) { 
   // language has single digits before 10s
    sc_sess->playFile(basedir+int2str(num.quot * 10)+suffix, false);
    sc_sess->playFile(basedir+(int2str(num.rem)+"-and")+suffix, false);
  } else {
    // language has single digits before 10s
    sc_sess->playFile(basedir+(int2str(num.rem)+"-and")+suffix, false);
    sc_sess->playFile(basedir+int2str(num.quot * 10)+suffix, false);
  }

  return false;
}

static int encryptDataAES128CBC(unsigned char *plaintext, int plaintextLen, unsigned char *key,
            unsigned char *iv, unsigned char *ciphertext)
{
    EVP_CIPHER_CTX *ctx;
    int len;
    int ciphertextLen;

    /* Create and initialise the context */
    if(!(ctx = EVP_CIPHER_CTX_new()))
        return -1;

    /* Initialise the encryption operation. IMPORTANT - ensure you use a key
     * and IV size appropriate for your cipher
     * In this example we are using 256 bit AES (i.e. a 256 bit key). The
     * IV size for *most* modes is the same as the block size. For AES this
     * is 128 bits
     */
    if(1 != EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv))
        return -1;

    /* Provide the message to be encrypted, and obtain the encrypted output.
     * EVP_EncryptUpdate can be called multiple times if necessary
     */
    if(1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintextLen))
        return -1;
    ciphertextLen = len;

    /* Finalise the encryption. Further ciphertext bytes may be written at
     * this stage.
     */
    if(1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))
        return -1;
    ciphertextLen += len;

    /* Clean up */
    EVP_CIPHER_CTX_free(ctx);

    return ciphertextLen;
}

static int decryptDataAES128CBC(unsigned char *ciphertext, int ciphertextLen, unsigned char *key,
            unsigned char *iv, unsigned char *plaintext)
{
    EVP_CIPHER_CTX *ctx;
    int len;
    int ciplaintextLen;

    /* Create and initialise the context */
    if(!(ctx = EVP_CIPHER_CTX_new()))
        return -1;

    /* Initialise the decryption operation. IMPORTANT - ensure you use a key
     * and IV size appropriate for your cipher
     * In this example we are using 256 bit AES (i.e. a 256 bit key). The
     * IV size for *most* modes is the same as the block size. For AES this
     * is 128 bits
     */
    if(1 != EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv))
        return -1;

    /* Provide the message to be decrypted, and obtain the plaintext output.
     * EVP_DecryptUpdate can be called multiple times if necessary.
     */
    if(1 != EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertextLen))
        return -1;
    ciplaintextLen = len;

    /* Finalise the decryption. Further plaintext bytes may be written at
     * this stage.
     */
    if(1 != EVP_DecryptFinal_ex(ctx, plaintext + len, &len))
        return -1;

    ciplaintextLen += len;

    /* Clean up */
    EVP_CIPHER_CTX_free(ctx);

    return ciplaintextLen;
}

static void removeFromString(string& value, const string & pattern) {
  size_t pos = std::string::npos;

  while ((pos = value.find(pattern)) != std::string::npos)
    value.erase(pos, pattern.length());
}

/**
 * base64
 */

const static char* b64="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" ;

/**
 * maps A=>0,B=>1..
 */
const static unsigned char unb64[]={
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //10
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //20
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //30
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //40
  0,   0,   0,  62,   0,   0,   0,  63,  52,  53, //50
 54,  55,  56,  57,  58,  59,  60,  61,   0,   0, //60
  0,   0,   0,   0,   0,   0,   1,   2,   3,   4, //70
  5,   6,   7,   8,   9,  10,  11,  12,  13,  14, //80
 15,  16,  17,  18,  19,  20,  21,  22,  23,  24, //90
 25,   0,   0,   0,   0,   0,   0,  26,  27,  28, //100
 29,  30,  31,  32,  33,  34,  35,  36,  37,  38, //110
 39,  40,  41,  42,  43,  44,  45,  46,  47,  48, //120
 49,  50,  51,   0,   0,   0,   0,   0,   0,   0, //130
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //140
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //150
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //160
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //170
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //180
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //190
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //200
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //210
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //220
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //230
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //240
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0, //250
  0,   0,   0,   0,   0,   0, 
}; /* This array has 255 elements */

/* Converts binary data of length=len to base64 characters.
 * Length of the resultant string is stored in flen
 * (you must pass pointer flen). */
static char* base64( const void* binaryData, int len, int *flen )
{
  const unsigned char* bin = (const unsigned char*) binaryData ;
  char* res ;

  int rc = 0 ; // result counter
  int byteNo ; // I need this after the loop

  int modulusLen = len % 3 ;
  int pad = ((modulusLen&1)<<1) + ((modulusLen&2)>>1) ; // 2 gives 1 and 1 gives 2, but 0 gives 0.

  *flen = 4*(len + pad)/3 ;
  res = (char*) malloc( *flen + 1 ) ; // and one for the null
  if( !res )
  {
    /* puts( "ERROR: base64 could not allocate enough memory." ) ; */
    /* puts( "I must stop because I could not get enough" ) ; */
    return 0;
  }

  for( byteNo = 0 ; byteNo <= len-3 ; byteNo+=3 )
  {
    unsigned char BYTE0=bin[byteNo];
    unsigned char BYTE1=bin[byteNo+1];
    unsigned char BYTE2=bin[byteNo+2];
    res[rc++]  = b64[ BYTE0 >> 2 ] ;
    res[rc++]  = b64[ ((0x3&BYTE0)<<4) + (BYTE1 >> 4) ] ;
    res[rc++]  = b64[ ((0x0f&BYTE1)<<2) + (BYTE2>>6) ] ;
    res[rc++]  = b64[ 0x3f&BYTE2 ] ;
  }

  if( pad==2 )
  {
    res[rc++] = b64[ bin[byteNo] >> 2 ] ;
    res[rc++] = b64[ (0x3&bin[byteNo])<<4 ] ;
    res[rc++] = '=';
    res[rc++] = '=';
  }
  else if( pad==1 )
  {
    res[rc++]  = b64[ bin[byteNo] >> 2 ] ;
    res[rc++]  = b64[ ((0x3&bin[byteNo])<<4)   +   (bin[byteNo+1] >> 4) ] ;
    res[rc++]  = b64[ (0x0f&bin[byteNo+1])<<2 ] ;
    res[rc++] = '=';
  }

  res[rc]=0; /* NULL TERMINATOR */
  return res ;
}

static unsigned char* unbase64( const char* ascii, int len, int *flen )
{
  const unsigned char *safeAsciiPtr = (const unsigned char*)ascii ;
  unsigned char *bin ;
  int cb=0;
  int charNo;
  int pad = 0 ;

  if( len < 2 ) { /* 2 accesses below would be OOB. */
    /* catch empty string, return NULL as result. */
    puts( "ERROR: You passed an invalid base64 string (too short). You get NULL back." ) ;
    *flen=0;
    return 0 ;
  }

  if( safeAsciiPtr[ len-1 ]=='=' )  ++pad ;
  if( safeAsciiPtr[ len-2 ]=='=' )  ++pad ;

  *flen = 3*len/4 - pad ;
  bin = (unsigned char*)malloc( *flen ) ;
  if( !bin )
  {
    /* puts( "ERROR: unbase64 could not allocate enough memory." ) ; */
    /* puts( "I must stop because I could not get enough" ) ; */
    return 0;
  }

  for( charNo=0; charNo <= len - 4 - pad ; charNo+=4 )
  {
    int A=unb64[safeAsciiPtr[charNo]];
    int B=unb64[safeAsciiPtr[charNo+1]];
    int C=unb64[safeAsciiPtr[charNo+2]];
    int D=unb64[safeAsciiPtr[charNo+3]];

    bin[cb++] = (A<<2) | (B>>4) ;
    bin[cb++] = (B<<4) | (C>>2) ;
    bin[cb++] = (C<<6) | (D) ;
  }

  if( pad==1 )
  {
    int A=unb64[safeAsciiPtr[charNo]];
    int B=unb64[safeAsciiPtr[charNo+1]];
    int C=unb64[safeAsciiPtr[charNo+2]];

    bin[cb++] = (A<<2) | (B>>4) ;
    bin[cb++] = (B<<4) | (C>>2) ;
  }
  else if( pad==2 )
  {
    int A=unb64[safeAsciiPtr[charNo]];
    int B=unb64[safeAsciiPtr[charNo+1]];

    bin[cb++] = (A<<2) | (B>>4) ;
  }

  return bin ;
}

CONST_ACTION_2P(SCUPlayCountRightAction, ',', true);
EXEC_ACTION_START(SCUPlayCountRightAction) {
  string cnt_s = resolveVars(par1, sess, sc_sess, event_params);
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int cnt = 0;
  if (str2int(cnt_s,cnt)) {
    ERROR("could not parse count '%s'\n", cnt_s.c_str());
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("could not parse count '"+cnt_s+"'\n");
    return false;
  }

  utils_play_count(sc_sess, cnt, basedir, ".wav", true);

  sc_sess->CLR_ERRNO;
} EXEC_ACTION_END;


CONST_ACTION_2P(SCUPlayCountLeftAction, ',', true);
EXEC_ACTION_START(SCUPlayCountLeftAction) {
  string cnt_s = resolveVars(par1, sess, sc_sess, event_params);
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int cnt = 0;
  if (str2int(cnt_s,cnt)) {
    ERROR("could not parse count '%s'\n", cnt_s.c_str());
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("could not parse count '"+cnt_s+"'\n");
    return false;
  }

  utils_play_count(sc_sess, cnt, basedir, ".wav", false);
  sc_sess->CLR_ERRNO;
} EXEC_ACTION_END;


CONST_ACTION_2P(SCUGetCountRightAction, ',', true);
EXEC_ACTION_START(SCUGetCountRightAction) {
  string cnt_s = resolveVars(par1, sess, sc_sess, event_params);
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int cnt = 0;
  if (str2int(cnt_s,cnt)) {
    ERROR("could not parse count '%s'\n", cnt_s.c_str());
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("could not parse count '"+cnt_s+"'\n");
    return false;
  }

  vector<string> filenames = utils_get_count_files(sc_sess, cnt, basedir, ".wav", true);

  cnt=0;
  for (vector<string>::iterator it=filenames.begin();it!=filenames.end();it++) {
    sc_sess->var["count_file["+int2str(cnt)+"]"]=*it;
    cnt++;
  }

  sc_sess->CLR_ERRNO;
} EXEC_ACTION_END;


CONST_ACTION_2P(SCUGetCountLeftAction, ',', true);
EXEC_ACTION_START(SCUGetCountLeftAction) {
  string cnt_s = resolveVars(par1, sess, sc_sess, event_params);
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int cnt = 0;
  if (str2int(cnt_s,cnt)) {
    ERROR("could not parse count '%s'\n", cnt_s.c_str());
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("could not parse count '"+cnt_s+"'\n");
    return false;
  }

  vector<string> filenames = utils_get_count_files(sc_sess, cnt, basedir, ".wav", false);

  cnt=0;
  for (vector<string>::iterator it=filenames.begin();it!=filenames.end();it++) {
    sc_sess->var["count_file["+int2str(cnt)+"]"]=*it;
    cnt++;
  }

  sc_sess->CLR_ERRNO;
} EXEC_ACTION_END;


CONST_ACTION_2P(SCUGetCountRightNoSuffixAction, ',', true);
EXEC_ACTION_START(SCUGetCountRightNoSuffixAction) {
  string cnt_s = resolveVars(par1, sess, sc_sess, event_params);
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int cnt = 0;
  if (str2int(cnt_s,cnt)) {
    ERROR("could not parse count '%s'\n", cnt_s.c_str());
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("could not parse count '"+cnt_s+"'\n");
    return false;
  }

  vector<string> filenames = utils_get_count_files(sc_sess, cnt, basedir, "", true);

  cnt=0;
  for (vector<string>::iterator it=filenames.begin();it!=filenames.end();it++) {
    sc_sess->var["count_file["+int2str(cnt)+"]"]=*it;
    cnt++;
  }

  sc_sess->CLR_ERRNO;
} EXEC_ACTION_END;


CONST_ACTION_2P(SCUGetCountLeftNoSuffixAction, ',', true);
EXEC_ACTION_START(SCUGetCountLeftNoSuffixAction) {
  string cnt_s = resolveVars(par1, sess, sc_sess, event_params);
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int cnt = 0;
  if (str2int(cnt_s,cnt)) {
    ERROR("could not parse count '%s'\n", cnt_s.c_str());
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("could not parse count '"+cnt_s+"'\n");
    return false;
  }

  vector<string> filenames = utils_get_count_files(sc_sess, cnt, basedir, "", false);

  cnt=0;
  for (vector<string>::iterator it=filenames.begin();it!=filenames.end();it++) {
    sc_sess->var["count_file["+int2str(cnt)+"]"]=*it;
    cnt++;
  }

  sc_sess->CLR_ERRNO;
} EXEC_ACTION_END;


CONST_ACTION_3P(SCUGetStringToCharAction, ',', true);
EXEC_ACTION_START(SCUGetStringToCharAction) {
  unsigned int cnt = 0;
  bool lower = false;

  string str = resolveVars(par1, sess, sc_sess, event_params);

  if (!par2.length()) {
    ERROR("name of the destination array nor specified\n");
    sc_sess->SET_ERRNO(DSM_ERRNO_UNKNOWN_ARG);
    sc_sess->SET_STRERROR("name of the destination array nor specified\n");
    return false;
  }
  string dst_array = (par2[0] == '$') ? par2.substr(1) : par2;

  if (par3.length() && par3 == "true")
    lower = true;

  DBG("splitting to chars the string '%s'\n", str.c_str());

  for (size_t i=0;i<str.length();i++) {
    sc_sess->var[dst_array+"["+int2str(cnt)+"]"] = (lower) ? tolower(str[i]) : str[i];
    cnt++;
  }

} EXEC_ACTION_END;


EXEC_ACTION_START(SCGetNewIdAction) {
  string d = resolveVars(arg, sess, sc_sess, event_params);
  sc_sess->var[d]=AmSession::getNewId();
} EXEC_ACTION_END;

CONST_ACTION_2P(SCURandomAction, ',', true);
EXEC_ACTION_START(SCURandomAction) {
  string varname = resolveVars(par1, sess, sc_sess, event_params);
  string modulo_str = resolveVars(par2, sess, sc_sess, event_params);

  unsigned int modulo = 0;
  if (modulo_str.length()) 
    str2int(modulo_str, modulo);
  
  if (modulo)
    sc_sess->var[varname]=int2str(rand()%modulo);
  else
    sc_sess->var[varname]=int2str(rand());

  DBG("Generated random $%s=%s\n", varname.c_str(), sc_sess->var[varname].c_str());
} EXEC_ACTION_END;

EXEC_ACTION_START(SCUSRandomAction) {
  srand(time(0));
} EXEC_ACTION_END;

CONST_ACTION_2P(SCUSpellAction, ',', true);
EXEC_ACTION_START(SCUSpellAction) {
  string basedir = resolveVars(par2, sess, sc_sess, event_params);

  string play_string = resolveVars(par1, sess, sc_sess, event_params);
  DBG("spelling '%s'\n", play_string.c_str());
  for (size_t i=0;i<play_string.length();i++)
    sc_sess->playFile(basedir+play_string[i]+".wav", false);

} EXEC_ACTION_END;


CONST_ACTION_2P(SCUSAddAction, ',', false);
EXEC_ACTION_START(SCUSAddAction) {
  string n1 = resolveVars(par1, sess, sc_sess, event_params);
  string n2 = resolveVars(par2, sess, sc_sess, event_params);

  string varname = par1;
  if (varname.length() && varname[0] == '$')
    varname = varname.substr(1);

  // todo: err checking
  string res = double2str(atof(n1.c_str()) + atof(n2.c_str()));

  DBG("setting var[%s] = %s + %s = %s\n", 
      varname.c_str(), n1.c_str(), n2.c_str(), res.c_str());
  sc_sess->var[std::move(varname)] = std::move(res);

} EXEC_ACTION_END;

CONST_ACTION_2P(SCUSSubAction, ',', false);
EXEC_ACTION_START(SCUSSubAction) {
  string n1 = resolveVars(par1, sess, sc_sess, event_params);
  string n2 = resolveVars(par2, sess, sc_sess, event_params);

  string varname = par1;
  if (varname.length() && varname[0] == '$')
    varname = varname.substr(1);

  // todo: err checking
  string res = double2str(atof(n1.c_str()) - atof(n2.c_str()));

  DBG("setting var[%s] = %s - %s = %s\n", 
      varname.c_str(), n1.c_str(), n2.c_str(), res.c_str());
  sc_sess->var[std::move(varname)] = std::move(res);

} EXEC_ACTION_END;

CONST_ACTION_2P(SCUIntAction, ',', false);
EXEC_ACTION_START(SCUIntAction) {
  string val = resolveVars(par2, sess, sc_sess, event_params);

  string varname = par1;
  if (varname.length() && varname[0] == '$')
    varname = varname.substr(1);  

  sc_sess->var[varname] = int2str((int)atof(val.c_str()));
  DBG("set $%s = %s\n", 
      varname.c_str(), sc_sess->var[varname].c_str());

} EXEC_ACTION_END;

CONST_ACTION_2P(SCUSplitStringAction, ',', true);
EXEC_ACTION_START(SCUSplitStringAction) {
  size_t cntr = 0;
  string str = resolveVars(par1, sess, sc_sess, event_params);
  string dst_array = par2;
  if (!dst_array.length())
    dst_array = par1;
  if (dst_array.length() && dst_array[0]=='$')
    dst_array = dst_array.substr(1);
  
  size_t p = 0, last_p = 0;
  while (true) {
    p = str.find("\n", last_p);
    if (p==string::npos) {
      if (last_p < str.length())
	sc_sess->var[dst_array+"["+int2str((unsigned int)cntr)+"]"] = str.substr(last_p);
      break;
    }
    
    sc_sess->var[dst_array+"["+int2str((unsigned int)cntr++)+"]"] = str.substr(last_p, p-last_p);

    last_p = p+1;    
  }
} EXEC_ACTION_END;

EXEC_ACTION_START(SCUEscapeCRLFAction) {
  string varname = arg;
  if (varname.length() && varname[0]=='$')
    varname.erase(0,1);

  while (true) {
    size_t p = sc_sess->var[varname].find("\r\n");
    if (p==string::npos)
      break;
    sc_sess->var[varname].replace(p, 2, "\\r\\n");
  }

  DBG("escaped: $%s='%s'\n", varname.c_str(), sc_sess->var[varname].c_str());
} EXEC_ACTION_END;


EXEC_ACTION_START(SCUUnescapeCRLFAction) {
  string varname = arg;
  if (varname.length() && varname[0]=='$')
    varname.erase(0,1);

  while (true) {
    size_t p = sc_sess->var[varname].find("\\r\\n");
    if (p==string::npos)
      break;
    sc_sess->var[varname].replace(p, 4, "\r\n");
  }

  DBG("unescaped: $%s='%s'\n", varname.c_str(), sc_sess->var[varname].c_str());
} EXEC_ACTION_END;


CONST_ACTION_2P(SCUPlayRingToneAction, ',', true);
EXEC_ACTION_START(SCUPlayRingToneAction) {

  int length = 0;
  int rtparams[4] = {2000, 4000, 440, 480}; // default values: ms, ms, Hz, Hz
  string s_length = resolveVars(par1, sess, sc_sess, event_params);

  if (!str2int(s_length, length)) {
    WARN("could not decipher ringtone length: '%s'\n", s_length.c_str());
  }

  /* it handles > 2 parameters case using explode per coma */
  vector<string> r_params = explode(par2, ",");
  for (vector<string>::iterator it=r_params.begin(); it !=r_params.end(); it++)
  {
    /* multi-arguments list is passed as one string, make sure to remove trash */
    removeFromString(*it, " ");
    removeFromString(*it, "\"");

    string p = resolveVars(*it, sess, sc_sess, event_params);

    if (p.empty())
      continue;

    if (!str2int(p, rtparams[it-r_params.begin()])) {
      WARN("could not decipher ringtone parameter %zd: '%s', using default\n", (it-r_params.begin()), p.c_str());
      continue;
    }
  }

  DBG("Playing ringtone length %d, on %d, off %d, f %d, f2 %d\n",
      length, rtparams[0], rtparams[1], rtparams[2], rtparams[3]);

  DSMRingTone* rt = new DSMRingTone(length, rtparams[0], rtparams[1], rtparams[2], rtparams[3]);
  sc_sess->addToPlaylist(new AmPlaylistItem(rt, NULL));

  sc_sess->transferOwnership(rt);
} EXEC_ACTION_END;

CONST_CONDITION_2P(IsInListCondition, ',', false);
MATCH_CONDITION_START(IsInListCondition) {
  string key = resolveVars(par1, sess, sc_sess, event_params);
  string cslist = resolveVars(par2, sess, sc_sess, event_params);
  DBG("checking whether '%s' is in list '%s'\n", key.c_str(), cslist.c_str());

  bool res = false;
  vector<string> items = explode(cslist, ",");
  for (vector<string>::iterator it=items.begin(); it != items.end(); it++) {
    if (key == trim(*it, " \t")) {
      res = true;
      break;
    }
  }
  DBG("key %sfound\n", res?"":" not");

  if (inv) {
    return !res;
  } else {
    return res;
  }
 } MATCH_CONDITION_END;

CONST_ACTION_2P(SCEncryptCodeAes128CBC, ',', true);
EXEC_ACTION_START(SCEncryptCodeAes128CBC) {
  string source_name = (par1.length() && par1[0] == '$') ? par1.substr(1) : par1;
  string source_value = resolveVars(par1, sess, sc_sess, event_params);

  /* voucher_iv and voucher_key */
  vector<string> params = explode(par2, ",");

  if (params.size() < 2) {
    throw DSMException("encrypt", "cause", "Lack of arguments!\n");
  }

  if (params[0].empty() || params[1].empty()) {
    throw DSMException("encrypt", "cause", "Empty arguments: iv or key!\n");
  }

  if (source_value.empty()) {
    throw DSMException("encrypt", "cause", "Empty arguments: source value!\n");
  }

  /* multi-arguments list is passed as one string, make sure to remove trash */
  for (auto it = params.begin(); it != params.end(); ++it)
  {
    removeFromString(*it, " ");
    removeFromString(*it, "\"");
  }

  /* vector and key, cast right away for encryption needs */
  string rawIv = resolveVars(params[0], sess, sc_sess, event_params);
  string rawKey = resolveVars(params[1], sess, sc_sess, event_params);
  unsigned char * voucherIv = (unsigned char *)rawIv.c_str();
  unsigned char * voucherKey = (unsigned char *)rawKey.c_str();

  /* prepare plain code for encryption */
  unsigned char * plainCode = (unsigned char *)source_value.c_str();

  /* padding with default AES key length */
  int paddedLen = source_value.length() + AES_KEY_SIZE - (source_value.length() % AES_KEY_SIZE);

  /* a buffer for an encrypted data */
  unsigned char encryptedCode[paddedLen];

  /* base64() related things */
  int flen = 0;
  char * encryptedCodeBase64;
  int encryptedLen = encryptDataAES128CBC(plainCode, strlen((char *)plainCode),
                              voucherKey, voucherIv, encryptedCode);

  if (encryptedLen == -1) {
    throw DSMException("encrypt", "cause", "Encrypt failure!\n");
  /* format into base64  */
  } else {
    encryptedCodeBase64 = base64(encryptedCode, paddedLen, &flen );
    DBG("Successfully encrypted the value: <%s>\n", encryptedCodeBase64);
    sc_sess->var[source_name] = encryptedCodeBase64;
  }

  /* base64() uses malloc, don't forget to free() */
  free(encryptedCodeBase64);
} EXEC_ACTION_END;

CONST_ACTION_2P(SCDecryptCodeAes128CBC, ',', true);
EXEC_ACTION_START(SCDecryptCodeAes128CBC) {
  /* encrypted code must be given in base64 */
  string source_name = (par1.length() && par1[0] == '$') ? par1.substr(1) : par1;
  string source_value = resolveVars(par1, sess, sc_sess, event_params);

  /* voucher_iv and voucher_key */
  vector<string> params = explode(par2, ",");

  if (params.size() < 2) {
    WARN("Lack of arguments!\n");
    throw DSMException("encrypt", "cause", "Lack of arguments!\n");
  }

  if (params[0].empty() || params[1].empty()) {
    WARN("Empty arguments: iv or key!\n");
    throw DSMException("encrypt", "cause", "Empty arguments: iv or key!\n");
  }

  if (source_value.empty()) {
    WARN("Empty arguments: source value!\n");
    throw DSMException("encrypt", "cause", "Empty arguments: source value!\n");
  }

  /* multi-arguments list is passed as one string, make sure to remove trash */
  for (auto it = params.begin(); it != params.end(); ++it)
  {
    removeFromString(*it, " ");
    removeFromString(*it, "\"");
  }

  /* vector and key, cast right away for encryption needs */
  string rawIv = resolveVars(params[0], sess, sc_sess, event_params);
  string rawKey = resolveVars(params[1], sess, sc_sess, event_params);

  unsigned char * voucherIv = (unsigned char *)rawIv.c_str();
  unsigned char * voucherKey = (unsigned char *)rawKey.c_str();

  /* base64() related things */
  int flen = 0;

  /* buffer for decryption, must be big enough */
  unsigned char decryptedCode[DEC_BUF_LEN];

  /* convert the encrypted code (in base64) into the raw binary data */
  unsigned char * encryptedCodeRaw = unbase64(source_value.c_str(), source_value.length(), &flen);

  int decrypteLen = decryptDataAES128CBC(encryptedCodeRaw, strlen((char *)encryptedCodeRaw),
                                voucherKey, voucherIv, decryptedCode);
  if (decrypteLen == -1) {
    throw DSMException("decrypt", "cause", "Decrypt failure!\n");
  } else {
    DBG("Successfully decrypted the value: <%s>\n", decryptedCode);
    sc_sess->var[source_name] = (char *)decryptedCode;
  }

  /* unbase64() uses malloc, don't forget to free() */
  free(encryptedCodeRaw);

} EXEC_ACTION_END;
