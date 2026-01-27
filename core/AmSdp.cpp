/*
 *parse or be parsed
 */


#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "AmConfig.h"
#include "AmSdp.h"
#include "AmUtils.h"
#include "AmPlugIn.h"
#include "AmSession.h"

#include "amci/amci.h"
#include "log.h"

#include <stdexcept>

#include <string>
#include <map>
using std::string;
using std::map;

#include <cctype>
#include <algorithm>

// Not on Solaris!
#if !defined (__SVR4) && !defined (__sun)
#include "strings.h"
#endif

#define CR   '\r'
#define LF   '\n'
#define CRLF "\r\n"

static void parse_session_attr(AmSdp* sdp_msg, const char* s, const char** next);
static bool parse_sdp_line_ex(AmSdp* sdp_msg, const char* s);
static const char* parse_sdp_connection(AmSdp* sdp_msg, const char* s, char t);
static void parse_sdp_media(AmSdp* sdp_msg, const char* s);
static const char* parse_sdp_attr(AmSdp* sdp_msg, const char* s);
static void parse_sdp_origin(AmSdp* sdp_masg, const char* s);

inline const char* get_next_line(const char* s);
inline const char* skip_till_next_line(const char* s, size_t& line_len);
static const char* is_eql_next(const char* s);
static const char* parse_until(const char* s, char end);
static const char* parse_until(const char* s, const char* end, char c);
static const char* parse_until(const char* s, const char* end, char c, char alt);
static bool contains(const char* s, const char* next_line, char c);
static bool is_wsp(char s);
const char* get_token(const char* s, const char* end, const char delim, const char delim_alt, string& token);

static MediaType media_type(const std::string& media);
static TransProt transport_type(const std::string& transport);
static bool attr_check(std::string attr);

enum parse_st {SDP_DESCR, SDP_MEDIA};
enum sdp_connection_st {NET_TYPE, ADDR_TYPE, IP4, IP6};
enum sdp_media_st {MEDIA, PORT, PROTO, FMT}; 
enum sdp_attr_rtpmap_st {TYPE, ENC_NAME, CLK_RATE, ENC_PARAM};
enum sdp_attr_fmtp_st {FORMAT, FORMAT_PARAM};
enum sdp_origin_st {USER, ID, VERSION_ST, NETTYPE, ADDR, UNICAST_ADDR};

// inline functions

inline string net_t_2_str(int nt)
{
  switch(nt){
  case NT_IN: return "IN";
  default: return "<unknown network type>";
  }
}

inline string addr_t_2_str(int at)
{
  switch(at){
  case AT_V4: return "IP4";
  case AT_V6: return "IP6";
  default: return "<unknown address type>";
  }
}


inline string media_t_2_str(const SdpMedia& m)
{
  switch(m.type){
  case MT_AUDIO: return "audio";
  case MT_VIDEO: return "video";
  case MT_APPLICATION: return "application";
  case MT_TEXT: return "text";
  case MT_MESSAGE: return "message";
  case MT_IMAGE: return "image";
  default: return m.type_str;
  }
}

inline string transport_p_2_str(const SdpMedia& m)
{
  switch(m.transport){
  case TP_RTPAVP: return "RTP/AVP";
  case TP_UDP: return "UDP";
  case TP_RTPSAVP: return "RTP/SAVP";
  case TP_UDPTL: return "udptl";
  case TP_RTPAVPF: return "RTP/AVPF";
  case TP_RTPSAVPF: return "RTP/SAVPF";
  case TP_UDPTLSRTPSAVP: return "UDP/TLS/RTP/SAVP";
  case TP_UDPTLSRTPSAVPF: return "UDP/TLS/RTP/SAVPF";

  default: return m.transport_str;
  }
}

bool SdpConnection::operator == (const SdpConnection& other) const
{
  return network == other.network && addrType == other.addrType 
    && address == other.address;
}

bool SdpOrigin::operator == (const SdpOrigin& other) const
{
  return user == other.user && sessId == other.sessId 
    && sessV == other.sessV && conn == other.conn;
}

bool SdpPayload::operator == (const SdpPayload& other) const
{
  return payload_type == other.payload_type && encoding_name == other.encoding_name 
    && clock_rate == other.clock_rate && encoding_param == other.encoding_param;
}

string SdpConnection::debugPrint() const {
  return addr_t_2_str(addrType) + " " + address;
}

bool SdpMedia::isRejected() const {
  return port == 0;
}

// hold: sendonly/inactive or IP 0.0.0.0
// vector<SdpAttribute>::const_iterator pos =
//   std::find(attributes.begin(), attributes.end(), SdpAttribute("sendonly"));
// return pos != attributes.end();

bool SdpMedia::haveNonTelevPayload() {
  // check if at least on non-telev payload matched
  if (payloads.empty() ||
      ((payloads.size() == 1) && (payloads[0].encoding_name == "telephone-event")))
    return false;
  
  return true;
}

string SdpMedia::debugPrint() const {
  string payload_list;
  for(std::vector<SdpPayload>::const_iterator it=
	payloads.begin(); it!= payloads.end(); it++) {
    if (it != payloads.begin())
      payload_list+=" ";
    payload_list+=int2str(it->payload_type);
  }
  return "port "+int2str(port) + ", payloads: "+payload_list;
}

string SdpMedia::type2str(const SdpMedia& m)
{
  return media_t_2_str(m);
}

bool SdpPayload::operator == (int r)
{
  DBG("pl == r: payload_type = %i; r = %i\n", payload_type, r);
  return payload_type == r;
}

string SdpAttribute::print() const
{
  if (value.empty())
    return "a="+attribute+CRLF;
  else
    return "a="+attribute+":"+value+CRLF;
}

bool SdpAttribute::operator==(const SdpAttribute& other) const
{
  return attribute==other.attribute && (value.empty() || (value==other.value));
}

bool SdpMedia::operator == (const SdpMedia& other) const
{
  if (payloads.empty()) {
    if (!other.payloads.empty())
      return false;

  } else if (other.payloads.empty()) {
    return false;

  } else {    
    std::pair<vector<SdpPayload>::const_iterator, vector<SdpPayload>::const_iterator> pl_mismatch 
      = std::mismatch(payloads.begin(), payloads.end(), other.payloads.begin());

    if (pl_mismatch.first != payloads.end() || pl_mismatch.second != other.payloads.end())
      return false;
  }

  if (attributes.empty()) {
    if (!other.attributes.empty()) {
      return false;
    }
  } else {
    std::pair<vector<SdpAttribute>::const_iterator, vector<SdpAttribute>::const_iterator> a_mismatch 
      = std::mismatch(attributes.begin(), attributes.end(), other.attributes.begin());

    if (a_mismatch.first != attributes.end() || a_mismatch.second != other.attributes.end())
      return false;
  }

  return type == other.type && port == other.port && nports == other.nports 
    && transport == other.transport && conn == other.conn && dir == other.dir
    && send == other.send && recv == other.recv;
}

/**
 * Get token until end, delim char or delim_alt hit, return advanced pointer
 */
const char* get_token(const char* s, const char* end, const char delim, const char delim_alt, string& token) {
  if (s==end) {
    token.clear();
    return NULL;
  }

  const char* n = parse_until(s, end, delim, delim_alt);
  token = string(s, (n-s)-1);

  return n;
}

//
// class RtcpAddress: Methods
//
bool RtcpAddress::parse(const string &src)
{
  port = 0;
  nettype.clear();
  addrtype.clear();
  address.clear();
  explicitPort = false;
  explicitAddress = false;

  int len = src.size();
  if (len < 1) return false;

  enum { PORT, NET_TYPE, ADDR_TYPE, ADDR } s = PORT;

  // parsing (somehow) according to RFC 3605
  //    rtcp-attribute =  "a=rtcp:" port  [nettype space addrtype space
  //                             connection-address] CRLF
  // nettype, addrtype is ignored
  for (int i = 0; i < len; ++i) {
    switch (s) {

      case (PORT):
        if (src[i] >= '0' && src[i] <= '9') {
	  port = port * 10 + (src[i] - '0');
	  explicitPort = true;
	}
        else if (src[i] == ' ') s = NET_TYPE;
        else if (src[i] == '\r') break;
        else return false; // error
        break;

      case NET_TYPE:
          if (src[i] == ' ') s = ADDR_TYPE;
          else nettype += src[i];
          break;

      case ADDR_TYPE:
          if (src[i] == ' ') s = ADDR;
          else addrtype += src[i];
          break;

      case ADDR:
          address += src[i];
	  explicitAddress = true;
          break;
    }
  }
  return s == PORT ||
    (s == ADDR && !address.empty()); 
  // FIXME: nettype, addrtype and addr should be verified
}

string RtcpAddress::print() const
{
  string s(int2str(port));
  if (!address.empty())
    s += " " + nettype + " " + addrtype + " " + address;
  return s;
}

string RtcpAddress::printExplicit() const
{
  string s(int2str(port));
  if (!address.empty() && explicitAddress)
    s += " " + nettype + " " + addrtype + " " + address;
  return s;
}

void RtcpAddress::setAddress(const SdpConnection &connection)
{
  nettype = net_t_2_str(connection.network);
  addrtype = addr_t_2_str(connection.addrType);
  address = connection.address;
}

RtcpAddress::RtcpAddress(const string &attr_value): port(0)
{
  if (!parse(attr_value)) 
    throw std::runtime_error("can't parse rtcp attribute value");
}

RtcpAddress::RtcpAddress(const SdpConnection &connection): port(0)
{
  setAddress(connection);
}


//
// class AmSdp: Methods
//
AmSdp::AmSdp()
  : origin(),
    sessionName(),
    conn(),
    media(),
    version(0)
{
  origin.user = "sems";
  origin.sessId = get_random();
  origin.sessV = get_random();
}

AmSdp::AmSdp(const AmSdp& p_sdp_msg)
  : version(p_sdp_msg.version),
    origin(p_sdp_msg.origin),
    sessionName(p_sdp_msg.sessionName),
    conn(p_sdp_msg.conn),
    media(p_sdp_msg.media),
    attributes(p_sdp_msg.attributes)
{
}

/**
 * Set the RTCP port to RTP port+1 if not explicitly specified in the SDP
 */
bool AmSdp::sdp_parse_rtcp()
{
  for(vector<SdpMedia>::iterator it = media.begin(); it != media.end(); ++it) {
    if (it->rtcp_mux) {
      /* set rtcp-address */
      it->rtcp_address = RtcpAddress(it->conn);
      DBG("Re-setting rtcp address to '%s'\n", it->conn.address.c_str());

      /* set rtcp-port */
      if (it->rtcp_address.getPort() <= 0)
      {
        if (it->port > 0) {
          it->rtcp_address.setPort(it->port);
          DBG("Re-setting rtcp port to sdp media provided one '%d'\n", it->port);
        }
        else {
          WARN("Unable to set rtcp port based on rtcp-mux connection!\n");
          return false; // failed, no sense to continue
        }
      }

    } else if (!it->rtcp_address.getPort()) {

      if (!it->rtcp_address.hasAddress()) {
        it->rtcp_address.setAddress(it->conn);
        DBG("Re-setting rtcp address to '%s'\n", it->conn.address.c_str());
      }

      it->rtcp_address.setPort(it->rtcp_address.getPort() + 1);
    }
  }
  return true;
}

/**
 * Check for session level 'a=ice-ufrag' and 'a=ice-passwd' attributes.
 * Set these values to each media stream in case they are not explicity defined.
 */
void AmSdp::sdp_parse_ice()
{
  if (hasAttribute("ice-ufrag") && hasAttribute("ice-pwd")) {
  
    string ice_username = getAttribute("ice-ufrag");
    string ice_password = getAttribute("ice-pwd");
    
    for (vector<SdpMedia>::iterator m_it = media.begin(); m_it != media.end(); ++m_it) {
      if (m_it->ice_username.empty() && m_it->ice_password.empty()) {
        m_it->ice_username = ice_username;
        m_it->ice_password = ice_password;
      }
    }
  }
}

/** Check for session level 'a=fingerprint' attribute.
 * Set DTLS values to each media stream in case they are not explicity defined.
 */
void AmSdp::sdp_parse_fingerprint()
{
  if (hasAttribute("fingerprint")) {
    string fingerprint = getAttribute("fingerprint");
    string dtls_hash, dtls_fingerprint;
    unsigned int pos = fingerprint.find(' ');

    if (pos != string::npos) {
      dtls_hash = fingerprint.substr(0, pos);
      dtls_fingerprint = fingerprint.substr(pos+1, fingerprint.size() - pos);
    }

    if (!dtls_hash.empty() && !dtls_fingerprint.empty()) {
      for (vector<SdpMedia>::iterator m_it = media.begin(); m_it != media.end(); ++m_it) {
        if (m_it->dtls_hash.empty() && m_it->dtls_fingerprint.empty()) {
          m_it->dtls_hash = dtls_hash;
          m_it->dtls_fingerprint = dtls_fingerprint;
        }
      }
    }
  }
}

/**
 * Save the ssrc value into each SdpMedia
 */
void AmSdp::sdp_parse_ssrc()
{
  for (vector<SdpMedia>::iterator m_it = media.begin(); m_it != media.end(); ++m_it) {

    if (!m_it->hasAttribute("ssrc")) continue;

    string value = m_it->getAttribute("ssrc");

    unsigned int pos = value.find(' ');
    if (pos != string::npos) {
      str2int(value.substr(0, pos), m_it->ssrc);
    }
  }
}

bool AmSdp::parse(const string& sdp_copy)
{
  if (sdp_copy.empty()) {
    WARN("Nothing to parse.\n");
    return false;
  }

  const char * sdp_msg = sdp_copy.c_str();
  if (!sdp_msg) {
    WARN("Cannot duplicate SDP for parsing.\n");
    return false;
  }

  /* remove all values already parsed before */
  clear();

  /* returns true if parsed normally,
   * parses such things as session/media connection as well*/
  bool parsed = parse_sdp_line_ex(this, sdp_msg);

  /* check session connection */
  if (parsed && conn.address.empty()) {

    /* session level conneciton is absent, last hope: now check media level(s) */
    bool media_conn_absent = false;

    for (vector<SdpMedia>::iterator it = media.begin();
         !media_conn_absent && (it != media.end());
         ++it)
    {
      /* if session connection was empty, check all medias now
       * if at least one media has no connection information, then
       * then consider it as falsy and stop iteration */
      media_conn_absent = it->conn.address.empty();
    }

    if (media_conn_absent) {
      WARN("SDP: Conn info absent on the session lvl and at least is also absent in one of the medias!\n");
      return false;
    }
  }

  parsed = sdp_parse_rtcp();
  sdp_parse_ice();
  sdp_parse_fingerprint();
  sdp_parse_ssrc();

  return parsed;
}

string SdpIceCandidate::print() const
{

  string buf = "a=candidate:";
  buf += candidate->foundation + " ";
  buf += int2str(candidate->component_id) + " ";
  buf += candidate->transport + " ";
  buf += int2str(candidate->priority) + " ";
  buf += candidate->address + " ";
  buf += int2str(candidate->port) + " ";
  buf += "typ " + candidate->type;
  buf += CRLF;

  return buf;
}

void AmSdp::print(string& body) const
{
  string out_buf = "v="+int2str(version)+"\r\n"
    "o="+origin.user+" "+int2str(origin.sessId)+" "+
    int2str(origin.sessV)+" IN ";

  if (!origin.conn.address.empty())
    if (origin.conn.address.find(".") != std::string::npos)
      out_buf += "IP4 " + origin.conn.address + "\r\n";
    else
      out_buf += "IP6 " + origin.conn.address + "\r\n";
  else if (!conn.address.empty())
    if (conn.address.find(".") != std::string::npos)
      out_buf += "IP4 " + conn.address + "\r\n";
    else
      out_buf += "IP6 " + conn.address + "\r\n";
  else if (media.size() && !media[0].conn.address.empty())
    if (media[0].conn.address.find(".") != std::string::npos)
      out_buf += "IP4 " + media[0].conn.address + "\r\n";
    else
      out_buf += "IP6 " + media[0].conn.address + "\r\n";
  else
    out_buf += "IP4 0.0.0.0\r\n";

  out_buf +=
    "s="+sessionName+"\r\n";
  if (!conn.address.empty()) {
    if (conn.address.find(".") != std::string::npos)
      out_buf += "c=IN IP4 ";
    else
      out_buf += "c=IN IP6 ";
    out_buf += conn.address + "\r\n";
  }

  out_buf += "t=0 0\r\n";

  // add attributes (session level)
  for (std::vector<SdpAttribute>::const_iterator a_it=
	 attributes.begin(); a_it != attributes.end(); a_it++) {
    out_buf += a_it->print();
  }

  for(std::vector<SdpMedia>::const_iterator media_it = media.begin();
      media_it != media.end(); media_it++) {
      
      out_buf += "m=" + media_t_2_str(*media_it) + " "
	+ int2str(media_it->port) + " " 
	+ transport_p_2_str(*media_it);

      string options;

      if (media_it->transport == TP_RTPAVP || media_it->transport == TP_RTPSAVP ||
	  media_it->transport == TP_RTPAVPF || media_it->transport == TP_RTPSAVPF ||
	  media_it->transport == TP_UDPTLSRTPSAVP || media_it->transport == TP_UDPTLSRTPSAVPF ) {
	for(std::vector<SdpPayload>::const_iterator pl_it = media_it->payloads.begin();
	    pl_it != media_it->payloads.end(); pl_it++) {

	  out_buf += " " + int2str(pl_it->payload_type);

	  // "a=rtpmap:" line
	  if (!pl_it->encoding_name.empty()) {
	    options += "a=rtpmap:" + int2str(pl_it->payload_type) + " "
	      + pl_it->encoding_name + "/" + int2str(pl_it->clock_rate);

	    if(pl_it->encoding_param > 0){
	      options += "/" + int2str(pl_it->encoding_param);
	    }

	    options += "\r\n";
	  }
	  
	  // "a=fmtp:" line
	  if(pl_it->sdp_format_parameters.size()){
	    options += "a=fmtp:" + int2str(pl_it->payload_type) + " "
	      + pl_it->sdp_format_parameters + "\r\n";
	  }
	  
	}

//  switch (media_it->dir) {
//  case SdpMedia::DirActive:  options += "a=direction:active\r\n"; break;
//  case SdpMedia::DirPassive: options += "a=direction:passive\r\n"; break;
//  case SdpMedia::DirBoth:  options += "a=direction:both\r\n"; break;
//  case SdpMedia::DirUndefined: break;
//  }
      }
      else {
        // for other transports (UDP/UDPTL) just print out fmt
        out_buf += " " + media_it->fmt;
        // ... and continue with c=, attributes, ...
      }

      if (!media_it->conn.address.empty())
        out_buf += "\r\nc=IN " + addr_t_2_str(media_it->conn.addrType) + 
		" " + media_it->conn.address;

      out_buf += "\r\n" + options;

      if(media_it->send){
	if(media_it->recv){
	  
	  if(!hasAttribute("sendrecv"))
	    out_buf += "a=sendrecv\r\n";

	} else if(!hasAttribute("sendonly")){

	  out_buf += "a=sendonly\r\n";
	}
      }
      else {
	if(media_it->recv) {

	  if(!hasAttribute("recvonly"))
	    out_buf += "a=recvonly\r\n";

	} else if(!hasAttribute("inactive")) {

	  out_buf += "a=inactive\r\n";
	}
      }

      // if we have a=rtcp parsed from an original SDP, print it back out as received
      if (media_it->rtcp_address_orig.isExplicit())
	out_buf += "a=rtcp:" + media_it->rtcp_address_orig.printExplicit() + CRLF;
      else if (media_it->rtcp_address.isExplicit())
	out_buf += "a=rtcp:" + media_it->rtcp_address.printExplicit() + CRLF;

      if (media_it->rtcp_mux)
	out_buf += "a=rtcp-mux\r\n";

      // add attributes (media level)
      for (std::vector<SdpAttribute>::const_iterator a_it=
	     media_it->attributes.begin(); a_it != media_it->attributes.end(); a_it++) {
	out_buf += a_it->print();
      }

      // add ICE credentials
      if (!media_it->ice_username.empty() && !media_it->ice_password.empty()) {
        out_buf += "a=ice-ufrag:" + media_it->ice_username + "\r\n";
        out_buf += "a=ice-pwd:" + media_it->ice_password + "\r\n";
      }

      // add ICE candidates
      for (std::vector<SdpIceCandidate>::const_iterator ice_it=media_it->iceCandidates.begin(); ice_it != media_it->iceCandidates.end(); ice_it++) {
        out_buf += ice_it->print();
      }
  }

  body = std::move(out_buf);
}

void AmSdp::addAttribute(SdpAttribute attr)
{
  attributes.push_back(attr);
}

bool AmSdp::hasAttribute(const string& name) const
{
  for (vector<SdpAttribute>::const_iterator it = attributes.begin();
        it!=attributes.end(); ++it) {
    if (it->attribute == name) return true; /* found */
  }
  return false;
}

string AmSdp::getAttribute(const string& name)
{
  //For each
  for (vector<SdpAttribute>::const_iterator it = attributes.begin(); it!=attributes.end(); ++it)
    //Check attribute
    if (it->attribute == name)
      //Return attribute value
      return it->value;
  //Not found
  return string();
}

bool AmSdp::removeAttribute(const string& name)
{
  int match=0;

  //For each
  for (vector<SdpAttribute>::iterator it = attributes.begin();
       it != attributes.end(); ++it) {

    //Check attribute
    if (it->attribute == name){
      match++;
      attributes.erase(it++);
      if(it == attributes.end())
	return match > 0;
    }
  }

  return match > 0;
}

const SdpPayload* AmSdp::telephoneEventPayload() const
{
  return findPayload("telephone-event");
}

const SdpPayload *AmSdp::findPayload(const string& name) const
{
  vector<SdpMedia>::const_iterator m_it;

  for (m_it = media.begin(); m_it != media.end(); ++m_it)
    {
      vector<SdpPayload>::const_iterator it = m_it->payloads.begin();
      for(; it != m_it->payloads.end(); ++it)

	{
	  if (it->encoding_name == name)
	    {
	      return new SdpPayload(*it);
	    }
	}
    }
  return NULL;
}

bool AmSdp::operator == (const AmSdp& other) const
{
  if (attributes.empty()) {
    if (!other.attributes.empty())
      return false;

  } else if (other.attributes.empty()) {
    return false;

  } else {
    std::pair<vector<SdpAttribute>::const_iterator, vector<SdpAttribute>::const_iterator> a_mismatch
      = std::mismatch(attributes.begin(), attributes.end(), other.attributes.begin());

    if (a_mismatch.first != attributes.end() || a_mismatch.second != other.attributes.end())
      return false;
  }
   
  if (media.empty()) {
    if (!other.media.empty())
      return false;

  } else if (other.media.empty()) {
    return false;

  } else {
    std::pair<vector<SdpMedia>::const_iterator, vector<SdpMedia>::const_iterator> m_mismatch
      = std::mismatch(media.begin(), media.end(), other.media.begin());

    if (m_mismatch.first != media.end() || m_mismatch.second != other.media.end())
      return false;
  }

  return version == other.version && origin == other.origin 
    && sessionName == other.sessionName && uri == other.uri && conn == other.conn;
}

const bool AmSdp::hasIce() const
{
  bool has = false;
  vector<SdpMedia>::const_iterator m_it;

  // Check for ice_ufrag and ice_pwd in all media streams
  for (m_it = media.begin(); m_it != media.end(); ++m_it) {
    if (m_it->port == 0)
      continue;

    if (m_it->ice_username.empty() || m_it->ice_password.empty()) {
      return false;
    } else {
      has = true;
    }
  }
  return has;
}

const bool AmSdp::hasDtls() const
{
  bool has = false;
  vector<SdpMedia>::const_iterator m_it;

  for (m_it = media.begin(); m_it != media.end(); ++m_it) {
    if (m_it->port == 0)
      continue;

    if (m_it->dtls_hash.empty() || m_it->dtls_fingerprint.empty()) {
      return false;
    } else {
      has = true;
    }
  }
  return has;
}

void AmSdp::clear()
{
  version = 0;
  origin  = SdpOrigin();
  sessionName.clear();
  uri.clear();
  conn = SdpConnection();
  attributes.clear();
  media.clear();
  l_origin = SdpOrigin();
}

void SdpMedia::calcAnswer(const AmPayloadProvider* payload_prov,
			  SdpMedia& answer) const
{
  if (!recv) answer.send = false;
  if (!send) answer.recv = false;

  switch (dir)
  {
    case SdpMedia::DirBoth:
      answer.dir = SdpMedia::DirBoth;
      break;
    case SdpMedia::DirActive:
      answer.dir = SdpMedia::DirPassive;
      break;
    case SdpMedia::DirPassive:
      answer.dir = SdpMedia::DirActive;
      break;
    case SdpMedia::DirUndefined:
      answer.dir = SdpMedia::DirUndefined;
      break;
  }

  /* Calculate the intersection with the offered set of payloads */
  vector<SdpPayload>::const_iterator it = payloads.begin();
  for(; it!= payloads.end(); ++it)
  {
    amci_payload_t* a_pl = NULL;
    if (it->payload_type < DYNAMIC_PAYLOAD_TYPE_START) {
      /* try static payloads */
      a_pl = payload_prov->payload(it->payload_type);
    }

    if (a_pl) {
      answer.payloads.push_back(SdpPayload(a_pl->payload_id,
                                a_pl->name,
                                a_pl->advertised_sample_rate,
                                0));
    } else {
      /* Try dynamic payloads
       * and give a chance to broken
       * implementation using a static payload number
       * for dynamic ones. */

      int int_pt = payload_prov->getDynPayload(it->encoding_name,
                                              it->clock_rate,
                                              it->encoding_param);
      if (int_pt != -1)
        answer.payloads.push_back(*it);
    }
  }
}

void SdpMedia::addAttribute(SdpAttribute attr)
{
  attributes.push_back(attr);
}

bool SdpMedia::removeAttribute(const string& name)
{
  int match=0;

  for (vector<SdpAttribute>::iterator it = attributes.begin();
        it != attributes.end(); ++it)
  {
    if (it->attribute == name) {
      match++;
      attributes.erase(it++);
      if (it == attributes.end())
        return match > 0;
    }
  }

  return match > 0;
}

bool SdpMedia::hasAttribute(const string& name) const
{
  for (vector<SdpAttribute>::const_iterator it = attributes.begin();
        it!=attributes.end(); ++it)
    if (it->attribute == name)
      return true;

  return false; /* Not found */
}

string SdpMedia::getAttribute(const string& name)
{
  for (vector<SdpAttribute>::const_iterator it = attributes.begin();
        it!=attributes.end(); ++it)
    if (it->attribute == name)
      return it->value;

  return string(); /* Not found */
}

/**
 * SDP body parser
 *
 * TODO: rework to deal with std::string and refactor user(s) accordingly.
 */
static bool parse_sdp_line_ex(AmSdp* sdp_msg, const char * s)
{
  /* SDP can't be empty */
  if (!s)
    return false;

  const char* next=0;
  size_t line_len = 0;
  parse_st state;

  /* default state */
  state=SDP_DESCR;
  DBG("Parsing SDP message.\n");

  while (*s != '\0')
  {
    switch (state) {

      case SDP_DESCR:
        switch (*s) {
          case 'v':
          {
            s = is_eql_next(s);
            next = skip_till_next_line(s, line_len);
            if (line_len) {
              string version(s, line_len);
              str2int(version, sdp_msg->version);
              // DBG("parse_sdp_line_ex: found version '%s'\n", version.c_str());
            } else {
              sdp_msg->version = 0;
            }
            s = next;
            state = SDP_DESCR;
            break;
          }

          case 'o':
            //DBG("parse_sdp_line_ex: found origin\n");
            s = is_eql_next(s);
            parse_sdp_origin(sdp_msg, s);
            s = get_next_line(s);
            state = SDP_DESCR;
            break;

          case 's':
          {
            s = is_eql_next(s);
            next = skip_till_next_line(s, line_len);
            if (line_len) {
              sdp_msg->sessionName = string(s, line_len);
            } else {
              sdp_msg->sessionName.clear();
            }
            s = next;
            break;
          }

          case 'u':
          {
            s = is_eql_next(s);
            next = skip_till_next_line(s, line_len);
            if (line_len) {
              sdp_msg->uri = string(s, line_len);
            } else {
              sdp_msg->uri.clear();
            }
            s = next;
          }
          break;

          case 'i':
          case 'e':
          case 'p':
          case 'b':
          case 't':
          case 'k':
            s = is_eql_next(s);
            s = skip_till_next_line(s, line_len);
            state = SDP_DESCR;
            break;
          case 'a':
            s = is_eql_next(s);
            parse_session_attr(sdp_msg, s, &next);
            // next = get_next_line(s);
            // parse_sdp_attr(sdp_msg, s);
            s = next;
            state = SDP_DESCR;
            break;
          case 'c':
            s = is_eql_next(s);
            s = parse_sdp_connection(sdp_msg, s, 'd');
            state = SDP_DESCR;
            break;
          case 'm':
            //DBG("parse_sdp_line_ex: found media\n");
            state = SDP_MEDIA;	
            break;

          default:
          {
            next = skip_till_next_line(s, line_len);
            if (line_len) {
              sdp_msg->uri = string(s, line_len);
            } else {
              sdp_msg->uri.clear();
            }

            next = skip_till_next_line(s, line_len);
            if (line_len) {
              DBG("parse_sdp_line: skipping unknown Session description %s=\n",
              string(s, line_len).c_str());
            }
            s = next;
            break;
          }
        }
        break;

      case SDP_MEDIA:
        switch (*s) {
          case 'm':
            s = is_eql_next(s);
            parse_sdp_media(sdp_msg, s);
            s = skip_till_next_line(s, line_len);
            state = SDP_MEDIA;
            break;

          case 'i':
            s = is_eql_next(s);
            s = skip_till_next_line(s, line_len);
            state = SDP_MEDIA;
            break;

          case 'c':
            s = is_eql_next(s);
            //DBG("parse_sdp_line: found media connection\n");
            s = parse_sdp_connection(sdp_msg, s, 'm');
            state = SDP_MEDIA;
            break;

          case 'b':
            s = is_eql_next(s);
            s = skip_till_next_line(s, line_len);
            state = SDP_MEDIA;
            break;

          case 'k':
            s = is_eql_next(s);
            s = skip_till_next_line(s, line_len);
            state = SDP_MEDIA;
            break;

          case 'a':
            s = is_eql_next(s);
            s = parse_sdp_attr(sdp_msg, s);
            state = SDP_MEDIA;
            break;

          default :
          {
            next = skip_till_next_line(s, line_len);
            if (line_len) {
              DBG("parse_sdp_line: skipping unknown Media description '%.*s'\n",
                  (int)line_len, s);
            }
            s = next;
            break;
          }
        }
        break;
    }
  }

  return true;
}

static const char* parse_sdp_connection(AmSdp* sdp_msg, const char* s, char t)
{
  const char* connection_line=s;
  const char* next=0;
  const char* next_line=0;

  size_t line_len = 0;
  int parsing=1;

  SdpConnection c;

  next_line = skip_till_next_line(s, line_len);
  if (line_len <= 7) { // should be at least c=IN IP4 ...
    DBG("short connection line '%.*s'\n", (int)line_len, s);
    return next_line;
  }

  sdp_connection_st state;
  state = NET_TYPE;

  //DBG("parse_sdp_line_ex: parse_sdp_connection: parsing sdp connection\n");

  while(parsing){
    switch(state){
    case NET_TYPE:
      //Ignore NET_TYPE since it is always IN, fixme
      c.network = NT_IN; // fixme
      connection_line +=3; // fixme
      state = ADDR_TYPE;
      break;
    case ADDR_TYPE:
      {
	string addr_type(connection_line,3);
	connection_line +=4; // fixme
	if(addr_type == "IP4"){
	  c.addrType = AT_V4;
	  state = IP4;
	}else if(addr_type == "IP6"){
	  c.addrType = AT_V6;
	  state = IP6;
	}else{
	  DBG("parse_sdp_connection: Unknown addr_type in c-line: '%s'\n", addr_type.c_str());
	  c.addrType = AT_NONE;
	  parsing = 0; // ???
	}
	break;
      }
    case IP4:
      {
	  if(contains(connection_line, next_line, '/')){
	      next = parse_until(s, '/');
	      c.address = string(connection_line,int(next-connection_line)-2);
	  }else{
	    c.address = string(connection_line, line_len-7);
	  }
	  parsing = 0;
	  break;
      }
      
    case IP6:
      { 
	  if(contains(connection_line, next_line, '/')){
	      next = parse_until(s, '/');
	      c.address = string(connection_line, int(next-connection_line)-2);
	  }else{
	      c.address = string(connection_line, line_len-7);
	  }
	  parsing = 0;
	  break;
      }
    }
  }
  if(t == 'd') {
    sdp_msg->conn = c;
    DBG("SDP: got session level connection: %s\n", c.debugPrint().c_str());
  } else if(t == 'm'){
    SdpMedia& media = sdp_msg->media.back();
    media.conn = c;
    DBG("SDP: got media level connection: %s\n", c.debugPrint().c_str());
  }

  //DBG("parse_sdp_line_ex: parse_sdp_connection: done parsing sdp connection\n");
  return next_line;
}

static void set_default_attrs(SdpMedia &m, AmSdp* sdp_msg)
{
  m.dir = SdpMedia::DirUndefined;

  // to avoid split-brain situation (like with media attributes) read from
  // attributes (=> unnecessary string comparisons)
  const vector<SdpAttribute> &attrs = sdp_msg->attributes;
  for (vector<SdpAttribute>::const_iterator i = attrs.begin(); i != attrs.end(); ++i) {
    if (i->attribute == "sendrecv") {
      m.send = true;
      m.recv = true;
    } else if (i->attribute == "sendonly") {
      m.send = true;
      m.recv = false;
    } else if (i->attribute == "recvonly") {
      m.send = false;
      m.recv = true;
    } else if (i->attribute == "inactive") {
      m.send = false;
      m.recv = false;
    }
  }
}

static void parse_sdp_media(AmSdp* sdp_msg, const char* s)
{
  SdpMedia m;
  
  sdp_media_st state;
  state = MEDIA;
  int parsing = 1;
  const char* media_line=s;
  const char* next=0;
  const char* line_end=0;
  line_end = get_next_line(media_line);
  SdpPayload payload;
  unsigned int payload_type;

  //DBG("parse_sdp_line_ex: parse_sdp_media: parsing media description...\n");
  // get default values from session-level attributes
  set_default_attrs(m, sdp_msg);

  while(parsing){
    switch(state){
    case MEDIA: 
      {
      next = parse_until(media_line, ' ');
      string media;
      if (next > media_line)
        media = string(media_line, int(next-media_line) - 1);

      m.type = media_type(media);

      if(m.type == MT_NONE) {
	ERROR("parse_sdp_media: Unknown media type\n");
      }
      media_line = next;
      state = PORT;
      break;
      }
    case PORT:
      {
      next = parse_until(media_line, ' ');
      //check for multiple ports
      if(contains(media_line, next, '/')){
	//port number
	next = parse_until(media_line, '/');
	string port;
	if (next > media_line)
	  port = string(media_line, int(next-media_line)-1);
	str2int(port, m.port);	
	//number of ports
	media_line = next;
	next = parse_until(media_line, ' ');
	string nports;
	if (next > media_line)
	  nports = string(media_line, int(next-media_line)-1);
	str2int(nports, m.nports);
      }else{
	//port number 
	next = parse_until(media_line, ' ');
	string port;
	if (next > media_line)
	  port = string(media_line, int(next-media_line)-1);
	str2int(port, m.port);
	media_line = next;
      }
      state = PROTO;
      break;
      }
    case PROTO:
      {
	next = parse_until(media_line, ' ');
	string proto;
	if (next > media_line)
	  proto = string(media_line, int(next-media_line)-1);
	// if(transport_type(proto) < 0){
	//   ERROR("parse_sdp_media: Unknown transport protocol\n");
	//   state = FMT;
	//   break;
	// }
	m.transport_str = proto;
	m.transport = transport_type(proto);
	if(m.transport == TP_NONE){
	  DBG("Unknown transport protocol: \"%s\"\n",proto.c_str());
	}
	media_line = next;
	state = FMT;
	break;
      }
    case FMT:
      {
	if (m.transport == TP_RTPAVP || m.transport == TP_RTPSAVP ||
	    m.transport == TP_RTPAVPF || m.transport == TP_RTPSAVPF ||
      m.transport == TP_UDPTLSRTPSAVP || m.transport == TP_UDPTLSRTPSAVPF) {
	  if (contains(media_line, line_end, ' ')) {
	    next = parse_until(media_line, ' ');
	    string value;
	    if (next > media_line)
	      value = string(media_line, int(next-media_line)-1);

	    if (!value.empty()) {
	      payload.type = m.type;
	      str2int(value, payload_type);
	      payload.payload_type = payload_type;
	      m.payloads.push_back(payload);
	    }
	    media_line = next;
	  } else {
	    string last_value;
	    if (line_end>media_line) {
	      if (*line_end == '\0') {
		// last line in message
		last_value = string(media_line, int(line_end-media_line));
	      } else {
		last_value = string(media_line, int(line_end-media_line)-1);
	      }
	    }
	    if (!last_value.empty()) {
	      payload.type = m.type;
	      str2int(last_value, payload_type);
	      payload.payload_type = payload_type;
	      m.payloads.push_back(payload);
	    }
	    parsing = 0;
	  }
	} else {
	  line_end--;
	  while (line_end > media_line &&
		 (*line_end == '\r' || *line_end == '\n'))
	    line_end--;
	  if (line_end>media_line)
	    m.fmt = string(media_line, line_end-media_line+1);
	  DBG("set media fmt to '%s'\n", m.fmt.c_str());
	  parsing = 0;
	}
      } break;
    }
  }
  sdp_msg->media.push_back(m);

  DBG("SDP: got media: %s\n", m.debugPrint().c_str());
  //DBG("parse_sdp_line_ex: parse_sdp_media: done parsing media description \n");
  return;
}

// session level attribute
static void parse_session_attr(AmSdp* sdp_msg, const char* s, const char** next) {
  size_t line_len = 0;
  *next = skip_till_next_line(s, line_len);
  if (*next == s) {
    WARN("premature end of SDP in session attr\n");
    while (**next != '\0') (*next)++;
    return;
  }
  const char* attr_end = *next-1;
  while (attr_end >= s &&
	 ((*attr_end == LF) || (*attr_end == CR)))
    attr_end--;

  if (*attr_end == ':') {
    WARN("incorrect SDP: value attrib without value: '%s'\n",
	 string(s, attr_end-s+1).c_str());
    return;
  }

  const char* col = parse_until(s, attr_end, ':');

  if (col == attr_end) {
    // property attribute
    sdp_msg->attributes.push_back(SdpAttribute(string(s, attr_end-s+1)));
    // DBG("got session attribute '%.*s\n", (int)(attr_end-s+1), s);
  } else {
    // value attribute
    sdp_msg->attributes.push_back(SdpAttribute(string(s, col-s-1),
					       string(col, attr_end-col+1)));
    // DBG("got session attribute '%.*s:%.*s'\n", (int)(col-s-1), s, (int)(attr_end-col+1), col);
  }
}

// media level attribute
static const char* parse_sdp_attr(AmSdp* sdp_msg, const char* s)
{
  if(sdp_msg->media.empty()){
    ERROR("While parsing media options: no actual media !\n");
    return s;
  }

  SdpMedia& media = sdp_msg->media.back();

  SdpPayload payload;

  sdp_attr_rtpmap_st rtpmap_st;
  sdp_attr_fmtp_st fmtp_st;
  rtpmap_st = TYPE;
  fmtp_st = FORMAT;
  const char* attr_line=s;
  const char* next=0;
  const char* line_end=0;
  size_t line_len = 0;
  int parsing = 1;
  line_end = skip_till_next_line(attr_line, line_len);

  unsigned int payload_type, clock_rate, encoding_param = 0;
  string encoding_name, params;

  string attr;
  if (!contains(attr_line, line_end, ':')) {
    attr = string(attr_line, line_len);
    attr_check(attr);
    parsing = 0;
  } else {
    next = parse_until(attr_line, ':');
    attr = string(attr_line, int(next-attr_line)-1);
    attr_line = next;
  }

  /* rtpmap */
  if (attr == "rtpmap") {
    while(parsing)
    {
      switch (rtpmap_st) {
        case TYPE:
        {
          next = parse_until(attr_line, ' ');
          string type(attr_line, int(next-attr_line)-1);
          str2int(type,payload_type);
          attr_line = next;
          rtpmap_st = ENC_NAME;
          break;
        }

        case ENC_NAME:
        {
          if (contains(s, line_end, '/')) {
            next = parse_until(attr_line, '/');
            string enc_name(attr_line, int(next-attr_line)-1);
            encoding_name = std::move(enc_name);
            attr_line = next;
            rtpmap_st = CLK_RATE;
            break;
          } else {
            rtpmap_st = ENC_PARAM;
            break;
          }
        }

        case CLK_RATE:
        {
          /* check for posible encoding parameters after clock rate */
          if (contains(attr_line, line_end, '/')) {
            next = parse_until(attr_line, '/');
            string clk_rate(attr_line, int(next-attr_line)-1);
            str2int(clk_rate, clock_rate);
            attr_line = next;
            rtpmap_st = ENC_PARAM;

          /* last line check */
          } else if (*line_end == '\0') {
            string clk_rate(attr_line, int(line_end-attr_line));
            str2int(clk_rate, clock_rate);
            parsing = 0;

          /* more lines to come */
          } else {
            string clk_rate(attr_line, int(line_end-attr_line)-1);
            str2int(clk_rate, clock_rate);
            parsing=0;
          }
          break;
        }

        case ENC_PARAM:
        {
          next = parse_until(attr_line, ' ');
          if (next < line_end) {
            string value(attr_line, int(next-attr_line)-1);
            str2int(value, encoding_param);
            attr_line = next;
            rtpmap_st = ENC_PARAM;
          } else {
            string last_value(attr_line, int(line_end-attr_line)-1);
            str2int(last_value, encoding_param);
            parsing = 0;
          }
          break;
        }
        break;
      }
    }

    DBG("found media attr 'rtpmap' type '%d'\n", payload_type);

    vector<SdpPayload>::iterator pl_it;

    /* find needed SDP payload type (one from m= line list) and point to it */
    for (pl_it=media.payloads.begin();
          (pl_it != media.payloads.end()) && (pl_it->payload_type != int(payload_type));
        ++pl_it);

    if (pl_it != media.payloads.end()) {
      *pl_it = SdpPayload( int(payload_type),
                  encoding_name,
                  int(clock_rate),
                  int(encoding_param));
    }

  /* fmtp */
  } else if (attr == "fmtp") {
    while (parsing)
    {
      switch (fmtp_st) { /* fixme */
        case FORMAT:
        {
          next = parse_until(attr_line, line_end, ' ');
          string fmtp_format(attr_line, int(next-attr_line)-1);
          str2int(fmtp_format, payload_type);
          attr_line = next;
          fmtp_st = FORMAT_PARAM;
          break;
        }

        case FORMAT_PARAM:
        {
          const char* param_end = line_end - 1;
          while (is_wsp(*param_end))
            param_end--;

          params = string(attr_line, param_end-attr_line+1);
          parsing = 0;
        }
        break;
      }
    }

    DBG("found media attr 'fmtp' for payload '%d': '%s'\n", payload_type, params.c_str());

    vector<SdpPayload>::iterator pl_it;

    /* find needed SDP payload type (one from m= line list) and point to it */
    for (pl_it=media.payloads.begin();
          (pl_it != media.payloads.end()) && (pl_it->payload_type != int(payload_type));
          pl_it++);

    if(pl_it != media.payloads.end())
      pl_it->sdp_format_parameters = params;

  /* direction */
  } else if (attr == "direction") {
    if (parsing) {
      size_t dir_len = 0;
      next = skip_till_next_line(attr_line, dir_len);
      string value(attr_line, dir_len);

      if (value == "active") {
        media.dir=SdpMedia::DirActive;
        // DBG("found media attr 'direction' value '%s'\n", (char*)value.c_str());
      } else if (value == "passive") {
        media.dir=SdpMedia::DirPassive;
        //DBG("found media attr 'direction' value '%s'\n", (char*)value.c_str());
      } else if (attr == "both") {
        media.dir=SdpMedia::DirBoth;
        //DBG("found media attr 'direction' value '%s'\n", (char*)value.c_str());
      } else {
        DBG("found unknown value for media attribute 'direction'\n");
      }
    } else {
      DBG("ignoring direction attribute without value\n");
    }

  /* sendrecv/sendonly/recvonly/inactive */
  } else if (attr == "sendrecv") {
    media.send = true;
    media.recv = true;
  } else if (attr == "sendonly") {
    media.send = true;
    media.recv = false;
  } else if (attr == "recvonly") {
    media.send = false;
    media.recv = true;
  } else if (attr == "inactive") {
    media.send = false;
    media.recv = false;

  /* rtcp */
  } else if (attr == "rtcp") {
    if (parsing) {
      next = parse_until(attr_line, '\r');
      if (next >= line_end) {
        DBG("found media attribute '%s', but value is not followed by cr\n",
            (char *)attr.c_str());
      }
      string value(attr_line, int(next-attr_line)-1);
      media.rtcp_address_orig = RtcpAddress(value);
      media.rtcp_address = media.rtcp_address_orig;

    }

  /* rtcp-mux */
  } else if (attr == "rtcp-mux") {
    media.rtcp_mux = true;
    DBG("rtcp-mux detected\n");

  /* ice-ufrag */
  } else if (attr == "ice-ufrag") {
    if (parsing) {
      next = parse_until(attr_line, '\r');
      if (next < line_end) {
        string value(attr_line, int(next-attr_line)-1);
        media.ice_username = value;
      } else {
        DBG("found media attribute 'ice-ufrag', but value is not followed by cr\n");
      }
    } else {
      DBG("ignoring ice-ufrag attribute without value\n");
    }

  /* ice-pwd */
  } else if (attr == "ice-pwd") {
    if (parsing) {
      next = parse_until(attr_line, '\r');
      if (next < line_end) {
        string value(attr_line, int(next-attr_line)-1);
        media.ice_password = value;
      } else {
        DBG("found media attribute 'ice-pwd', but value is not followed by cr\n");
      }
    } else {
      DBG("ignoring ice-pwd attribute without value\n");
    }

  /* fingerprint */
  } else if (attr == "fingerprint") {
    if (parsing) {
      next = parse_until(attr_line, '\r');
      if (next < line_end) {
        string value(attr_line, int(next-attr_line)-1);
        unsigned int pos = value.find(' ');
        if (pos != string::npos) {
          media.dtls_hash = value.substr(0, pos);
          media.dtls_fingerprint = value.substr(pos+1, value.size() - pos);
        }
      } else {
        DBG("found media attribute 'fingerprint', but value is not followed by cr\n");
      }
    } else {
      DBG("ignoring fingerprint attribute without value\n");
    }

  /* setup */
  } else if (attr == "setup") {
    if (parsing) {
      next = parse_until(attr_line, '\r');
      if (next < line_end){
        string value(attr_line, int(next-attr_line)-1);
        media.dtls_role = value;
      } else {
        DBG("found media attribute 'setup', but value is not followed by cr\n");
      }
    } else {
      DBG("ignoring setup attribute without value\n");
    }

  /* generic a= line */
  } else {
    attr_check(attr);
    string value;
    if (parsing) {
      size_t attr_len = 0;
      next = skip_till_next_line(attr_line, attr_len);
      value = string (attr_line, attr_len);
    }
    media.attributes.push_back(SdpAttribute(attr, value));
  }
  return line_end;
}


static void parse_sdp_origin(AmSdp* sdp_msg, const char* s)
{
  const char* origin_line = s;
  const char* next=0;
  const char* line_end=0;
  size_t line_len=0;
  line_end = skip_till_next_line(s, line_len);
  
  sdp_origin_st origin_st;
  origin_st = USER;
  int parsing = 1;
  
  SdpOrigin origin;

  //DBG("parse_sdp_line_ex: parse_sdp_origin: parsing sdp origin\n");

  while(parsing){
    switch(origin_st)
      {
      case USER:
	{
	  next = parse_until(origin_line, ' ');
	  if(next > line_end){
	    DBG("parse_sdp_origin: ST_USER: Incorrect number of value in o=\n");
	    origin_st = UNICAST_ADDR;
	    break;
	  }
	  string user(origin_line, int(next-origin_line)-1);
	  origin.user = std::move(user);
	  origin_line = next;
	  origin_st = ID;
	  break;
	}
      case ID:
	{
	  next = parse_until(origin_line, ' ');
	  if(next > line_end){
	    DBG("parse_sdp_origin: ST_ID: Incorrect number of value in o=\n");
	    origin_st = UNICAST_ADDR;
	    break;
	  }
	  string id(origin_line, int(next-origin_line)-1);
	  str2int(id, origin.sessId);
	  origin_line = next;
	  origin_st = VERSION_ST;
	  break;
	}
      case VERSION_ST:
	{
	  next = parse_until(origin_line, ' ');
	  if(next > line_end){
	    DBG("parse_sdp_origin: ST_VERSION: Incorrect number of value in o=\n");
	    origin_st = UNICAST_ADDR;
	    break;
	  }
	  string version(origin_line, int(next-origin_line)-1);
	  str2int(version, origin.sessV);
	  origin_line = next;
	  origin_st = NETTYPE;
	  break;
	}
      case NETTYPE:
	{
	  next = parse_until(origin_line, ' ');
	  if(next > line_end){
	    DBG("parse_sdp_origin: ST_NETTYPE: Incorrect number of value in o=\n");
	    origin_st = UNICAST_ADDR;
	    break;
	  }
	  string net_type(origin_line, int(next-origin_line)-1);
	  origin.conn.network = NT_IN; // fixme
	  origin_line = next;
	  origin_st = ADDR;
	  break;
	}
      case ADDR:
	{
       	  next = parse_until(origin_line, ' ');
	  if(next > line_end){
	    DBG("parse_sdp_origin: ST_ADDR: Incorrect number of value in o=\n");
	    origin_st = UNICAST_ADDR;
	    break;
	  }

	  string addr_type(origin_line, int(next-origin_line)-1);
	  if(addr_type == "IP4"){
	    origin.conn.addrType = AT_V4;
	  }else if(addr_type == "IP6"){
	    origin.conn.addrType = AT_V6;
	  }else{
	    DBG("parse_sdp_connection: Unknown addr_type in o line: '%s'\n", addr_type.c_str());
	    origin.conn.addrType = AT_NONE;
	  }

	  origin_line = next;
	  origin_st = UNICAST_ADDR;
	  break;
	}
      case UNICAST_ADDR:
	{
	  next = parse_until(origin_line, ' ');
	  if (next != origin_line) {
	    //check if line contains more values than allowed
	    if(next > line_end){
	      size_t addr_len = 0;
	      skip_till_next_line(origin_line, addr_len);
	      origin.conn.address = string(origin_line, addr_len);
	    }else{
	      DBG("parse_sdp_origin: 'o=' contains more values than allowed; these values will be ignored\n");  
	      origin.conn.address = string(origin_line, int(next-origin_line)-1);
	    }
	  } else {
	    origin.conn.address = "";
	  }
	  parsing = 0;
	  break;
	}
      }
  }
  
  sdp_msg->origin = origin;

  //DBG("parse_sdp_line_ex: parse_sdp_origin: done parsing sdp origin\n");
  return;
}

/*
 *HELPER FUNCTIONS
 */

static bool contains(const char* s, const char* next_line, char c)
{
  const char* line=s;
  while((line != next_line-1) && (*line)){
    if(*line == c)
      return true;
    line++;
  }
  return false;
}

static bool is_wsp(char s) {
  return s==' ' || s == '\r' || s == '\n' || s == '\t';
}

static const char* parse_until(const char* s, char end)
{
  const char* line=s;
  while(*line && *line != end ){
    line++;
  }
  line++;
  return line;
}

static const char* parse_until(const char* s, const char* end, char c)
{
  const char* line=s;
  while(line<end && *line && *line != c ){
    line++;
  }
  if (line<end)
    line++;
  return line;
}

/* parse until c or alt or end reached */
static const char* parse_until(const char* s, const char* end, char c, char alt)
{
  const char* line=s;
  while(line<end && *line && *line != c && *line != alt){
    line++;
  }
  if (line<end)
    line++;
  return line;
}

static size_t len_till_eol(char* s, char* end)
{
  size_t res=0;
  char* line=s;
  while(line<end && *line && *line != '\r' && *line != '\n'){
    line++;
    res++;
  }
  return res;
}

static size_t len_till_char_or_eol(char* s, char* end, char c)
{
  size_t res=0;
  char* line=s;
  while(line<end && *line && *line !=  c && *line != '\r' && *line != '\n'){
    line++;
    res++;
  }
  return res;
}

static const char* is_eql_next(const char* s)
{
  const char* next = s + 1;
  if (*next != '=') {
      DBG("parse_sdp_line: expected '=' but found <%c> \n", *next);
  }
  return next + 1;
}

inline const char* get_next_line(const char* s)
{
  const char* next_line=s;
  //search for next line
 while( *next_line != '\0') {
    if(*next_line == CR){
      next_line++;
      if (*next_line == LF) {
	next_line++;
	break;
      } else {
	continue;
      }
    } else if (*next_line == LF){	
      next_line++;
      break;
    }
    next_line++;
 }

  return next_line; 
}

/* skip to 0, CRLF or LF;
   @return line_len length of current line
   @return start of next line 
*/
inline const char* skip_till_next_line(const char* s, size_t& line_len)
{
  const char* next_line=s;
  line_len = 0;

  //search for next line
  while( *next_line != '\0') {
    if (*next_line == CR) {
      next_line++;
      if (*next_line == LF) {
	next_line++;
	break;
      } else {
	continue;
      }
    } else if (*next_line == LF){	
      next_line++;
      break;
    }
    line_len++;
    next_line++;
 }

  return next_line; 
}

/*
 *Check if known media type is used
 */
static MediaType media_type(const std::string& media)
{
  if(media == "audio")
    return MT_AUDIO;
  else if(media == "video")
    return MT_VIDEO;
  else if(media == "application")
    return MT_APPLICATION;
  else if(media == "text")
    return MT_TEXT;
  else if(media == "message")
    return MT_MESSAGE;
  else if(media == "image")
    return MT_IMAGE;
  else 
    return MT_NONE;
}

static TransProt transport_type(const string& transport)
{
  string transport_uc = transport;
  std::transform(transport_uc.begin(), transport_uc.end(), transport_uc.begin(), toupper);

  if(transport_uc == "RTP/AVP")
    return TP_RTPAVP;
  else if(transport_uc == "UDP")
    return TP_UDP;
  else if(transport_uc == "RTP/SAVP")
    return TP_RTPSAVP;
  else if(transport_uc == "UDPTL")
    return TP_UDPTL;
  else if(transport_uc == "RTP/AVPF")
    return TP_RTPAVPF;
  else if(transport_uc == "RTP/SAVPF")
    return TP_RTPSAVPF;
  else if(transport_uc == "UDP/TLS/RTP/SAVP")
    return TP_UDPTLSRTPSAVP;
  else if(transport_uc == "UDP/TLS/RTP/SAVPF")
    return TP_UDPTLSRTPSAVPF;
  else 
    return TP_NONE;
}


/*
*Check if known attribute name is used
*/
static bool attr_check(std::string attr)
{
  if(attr == "cat")
    return true;
  else if(attr == "keywds")
    return true;
  else if(attr == "tool")
    return true;
  else if(attr == "ptime")
    return true;
  else if(attr == "maxptime")
    return true;
  else if(attr == "recvonly")
    return true;
  else if(attr == "sendrecv")
    return true;
  else if(attr == "sendonly")
    return true;
  else if(attr == "inactive")
    return true;
  else if(attr == "orient")
    return true;
  else if(attr == "type")
    return true;
  else if(attr == "charset")
    return true;
  else if(attr == "sdplang")
    return true;
  else if(attr == "lang")
    return true;
  else if(attr == "framerate")
    return true;
  else if(attr == "quality")
    return true;
  else if(attr == "both")
    return true;
  else if(attr == "active")
    return true;
  else if(attr == "passive")
    return true;
  else if(attr == "rtcp")
    return true;
  else if(attr == "rtcp-mux")
    return true;
  else {
    DBG("unknown attribute: %s\n", (char*)attr.c_str());
    return false;
  }
}

