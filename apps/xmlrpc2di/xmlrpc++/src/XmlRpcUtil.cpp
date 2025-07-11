
#include "XmlRpcUtil.h"

#ifndef MAKEDEPEND
# include <ctype.h>
# include <iostream>
# include <stdarg.h>
# include <stdio.h>
# include <string.h>
#endif

#include "XmlRpc.h"

using namespace XmlRpc;


//#define USE_WINDOWS_DEBUG // To make the error and log messages go to VC++ debug output
#ifdef USE_WINDOWS_DEBUG
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Version id
const char XmlRpc::XMLRPC_VERSION[] = "XMLRPC++ 0.8";

// Default log verbosity: 0 for no messages through 5 (writes everything)
int XmlRpcLogHandler::_verbosity = 0;

// Default log handler
static class DefaultLogHandler : public XmlRpcLogHandler {
public:

  void log(int level, const char* msg) { 
#ifdef USE_WINDOWS_DEBUG
    if (level <= _verbosity) { OutputDebugString(msg); OutputDebugString("\n"); }
#else
    if (level <= _verbosity) std::cout << msg << std::endl; 
#endif  
  }

} defaultLogHandler;

// Message log singleton
XmlRpcLogHandler* XmlRpcLogHandler::_logHandler = &defaultLogHandler;


// Default error handler
static class DefaultErrorHandler : public XmlRpcErrorHandler {
public:

  void error(const char* msg) {
#ifdef USE_WINDOWS_DEBUG
    OutputDebugString(msg); OutputDebugString("\n");
#else
    std::cerr << msg << std::endl; 
#endif  
  }
} defaultErrorHandler;


// Error handler singleton
XmlRpcErrorHandler* XmlRpcErrorHandler::_errorHandler = &defaultErrorHandler;


// Easy API for log verbosity
int XmlRpc::getVerbosity() { return XmlRpcLogHandler::getVerbosity(); }
void XmlRpc::setVerbosity(int level) { XmlRpcLogHandler::setVerbosity(level); }

 

void XmlRpcUtil::log(int level, const char* fmt, ...)
{
  if (level <= XmlRpcLogHandler::getVerbosity())
  {
    va_list va;
    char buf[1024];
    va_start( va, fmt);
    vsnprintf(buf,sizeof(buf)-1,fmt,va);
    buf[sizeof(buf)-1] = 0;
    XmlRpcLogHandler::getLogHandler()->log(level, buf);
    va_end(va);
  }
}


void XmlRpcUtil::error(const char* fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  char buf[1024];
  vsnprintf(buf,sizeof(buf)-1,fmt,va);
  buf[sizeof(buf)-1] = 0;
  XmlRpcErrorHandler::getErrorHandler()->error(buf);
  va_end(va);
}


// Returns contents between <tag> and </tag>, updates offset to char after </tag>
std::string 
XmlRpcUtil::parseTag(const char* tag, std::string const& xml, size_t* offset)
{
  if (*offset >= xml.length()) return std::string();
  size_t istart = xml.find(tag, *offset);
  if (istart == std::string::npos) return std::string();
  istart += strlen(tag);
  std::string etag = "</";
  etag += tag + 1;
  size_t iend = xml.find(etag, istart);
  if (iend == std::string::npos) return std::string();

  *offset = iend + etag.length();
  return xml.substr(istart, iend-istart);
}


// Returns true if the tag is found and updates offset to the char after the tag
bool 
XmlRpcUtil::findTag(const char* tag, std::string const& xml, size_t* offset)
{
  if (*offset >= xml.length()) return false;
  size_t istart = xml.find(tag, *offset);
  if (istart == std::string::npos)
    return false;

  *offset = istart + strlen(tag);
  return true;
}


// Returns true if the tag is found at the specified offset (modulo any whitespace)
// and updates offset to the char after the tag
bool 
XmlRpcUtil::nextTagIs(const char* tag, std::string const& xml, size_t* offset)
{
  if (*offset >= xml.length()) return false;
  const char* cp = xml.c_str() + *offset;
  size_t nc = 0;
  while (*cp && isspace(*cp)) {
    ++cp;
    ++nc;
  }

  size_t len = strlen(tag);
  if  (*cp && (strncmp(cp, tag, len) == 0)) {
    *offset += nc + len;
    return true;
  }
  return false;
}

// Returns the next tag and updates offset to the char after the tag, or empty string
// if the next non-whitespace character is not '<'. Ignores parameters and values within
// the tag entity.
std::string 
XmlRpcUtil::getNextTag(std::string const& xml, size_t* offset)
{
  if (*offset >= xml.length()) return std::string();

  const char* cp = xml.c_str() + size_t(*offset);
  const char* startcp = cp;
  while (*cp && isspace(*cp))
    ++cp;


  if (*cp != '<') return std::string();

  // Tag includes the non-blank characters after <
  const char* start = cp++;
  while (*cp != '>' && *cp != 0 && ! isspace(*cp))
    ++cp;

  std::string s(start, cp-start+1);

  if (*cp != '>')   // Skip parameters and values
  {
    while (*cp != '>' && *cp != 0)
      ++cp;

    s[s.length()-1] = *cp;
  }

  *offset += cp - startcp + 1;
  return s;
}



// xml encodings (xml-encoded entities are preceded with '&')
static const char  AMP = '&';
static const char  rawEntity[] = { '<',   '>',   '&',    '\'',    '\"',    0 };
static const char* xmlEntity[] = { "lt;", "gt;", "amp;", "apos;", "quot;", 0 };
static const int   xmlEntLen[] = { 3,     3,     4,      5,       5 };


// Replace xml-encoded entities with the raw text equivalents.

std::string 
XmlRpcUtil::xmlDecode(const std::string& encoded)
{
  std::string::size_type iAmp = encoded.find(AMP);
  if (iAmp == std::string::npos)
    return encoded;

  std::string decoded(encoded, 0, iAmp);
  std::string::size_type iSize = encoded.size();
  decoded.reserve(iSize);

  const char* ens = encoded.c_str();
  while (iAmp != iSize) {
    if (encoded[iAmp] == AMP && iAmp+1 < iSize) {
      int iEntity;
      for (iEntity=0; xmlEntity[iEntity] != 0; ++iEntity)
	//if (encoded.compare(iAmp+1, xmlEntLen[iEntity], xmlEntity[iEntity]) == 0)
	if (strncmp(ens+iAmp+1, xmlEntity[iEntity], xmlEntLen[iEntity]) == 0)
        {
          decoded += rawEntity[iEntity];
          iAmp += xmlEntLen[iEntity]+1;
          break;
        }
      if (xmlEntity[iEntity] == 0)    // unrecognized sequence
        decoded += encoded[iAmp++];

    } else {
      decoded += encoded[iAmp++];
    }
  }
    
  return decoded;
}


// Replace raw text with xml-encoded entities.

std::string 
XmlRpcUtil::xmlEncode(const std::string& raw)
{
  std::string::size_type iRep = raw.find_first_of(rawEntity);
  if (iRep == std::string::npos)
    return raw;

  std::string encoded(raw, 0, iRep);
  std::string::size_type iSize = raw.size();

  while (iRep != iSize) {
    int iEntity;
    for (iEntity=0; rawEntity[iEntity] != 0; ++iEntity)
      if (raw[iRep] == rawEntity[iEntity])
      {
        encoded += AMP;
        encoded += xmlEntity[iEntity];
        break;
      }
    if (rawEntity[iEntity] == 0)
      encoded += raw[iRep];
    ++iRep;
  }
  return encoded;
}



