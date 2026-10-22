#include "fct.h"

#include "log.h"

#include "AmSdp.h"

#define CRLF "\r\n"
#define LF "\n"

FCTMF_SUITE_BGN(test_sdp) {

    FCT_TEST_BGN(normal_sdp_ok) {
      AmSdp s;
      string sdp = 
	"v=0" CRLF
	"o=- 3615077380 3615077398 IN IP4 178.66.14.5" CRLF
	"s=-" CRLF
	"c=IN IP4 178.66.14.5" CRLF
	"t=0 0" CRLF
	"m=audio 21964 RTP/AVP 0 101" CRLF
	"a=sendrecv" CRLF
	"a=ptime:20" CRLF
	"a=rtpmap:0 PCMU/8000" CRLF
	"a=rtpmap:101 telephone-event/8000" CRLF
	"a=fmtp:101 0-15" CRLF ;

      fct_chk(s.parse((char *)sdp.c_str()));
      fct_chk(s.version==0);
      fct_chk(s.origin.user == "-");
      fct_chk(s.origin.sessId == 3615077380);
      fct_chk(s.origin.sessV == 3615077398);
      fct_chk(s.origin.conn.address == "178.66.14.5");
      fct_chk(s.origin.conn.network == NT_IN);
      fct_chk(s.origin.conn.addrType == AT_V4);

      fct_chk(s.conn.address == "178.66.14.5");
      fct_chk(s.conn.network == NT_IN);
      fct_chk(s.conn.addrType == AT_V4);

      fct_chk(s.media.size() == 1);
      fct_chk(s.media[0].type == MT_AUDIO);
      fct_chk(s.media[0].port == 21964);
      fct_chk(s.media[0].transport == TP_RTPAVP);
      fct_chk(s.media[0].payloads.size()==2);
      fct_chk(s.media[0].payloads[0].payload_type==0);
      fct_chk(s.media[0].payloads[1].payload_type==101);
      fct_chk(s.media[0].payloads[0].encoding_name=="PCMU");
      fct_chk(s.media[0].payloads[1].encoding_name=="telephone-event");
    } FCT_TEST_END();

    FCT_TEST_BGN(sdp_LF_no_CRLF) {
      AmSdp s;
      string sdp = 
	"v=0" LF
	"o=- 3615077380 3615077398 IN IP4 178.66.14.5" LF
	"s=-" LF
	"c=IN IP4 178.66.14.5" LF
	"t=0 0" LF
	"m=audio 21964 RTP/AVP 0 101" LF
	"a=sendrecv" LF
	"a=ptime:20" LF
	"a=rtpmap:0 PCMU/8000" LF
	"a=rtpmap:101 telephone-event/8000" LF
	"a=fmtp:101 0-15" LF ;

      fct_chk(s.parse((char *)sdp.c_str()));
      fct_chk(s.version==0);
      fct_chk(s.origin.user == "-");
      fct_chk(s.origin.sessId == 3615077380);
      fct_chk(s.origin.sessV == 3615077398);
      fct_chk(s.origin.conn.address == "178.66.14.5");
      fct_chk(s.origin.conn.network == NT_IN);
      fct_chk(s.origin.conn.addrType == AT_V4);

      fct_chk(s.conn.address == "178.66.14.5");
      fct_chk(s.conn.network == NT_IN);
      fct_chk(s.conn.addrType == AT_V4);

      fct_chk(s.media.size() == 1);
      fct_chk(s.media[0].type == MT_AUDIO);
      fct_chk(s.media[0].port == 21964);
      fct_chk(s.media[0].transport == TP_RTPAVP);
      fct_chk(s.media[0].payloads.size()==2);
      fct_chk(s.media[0].payloads[0].payload_type==0);
      fct_chk(s.media[0].payloads[1].payload_type==101);
      fct_chk(s.media[0].payloads[0].encoding_name=="PCMU");
      fct_chk(s.media[0].payloads[1].encoding_name=="telephone-event");
    } FCT_TEST_END();

  /* a=rtcp of an incoming offer describes the remote party, so it must not be
   * printed back out once the media port has been replaced with a local one */
  FCT_TEST_BGN(sdp_replace_rtcp_address) {
    const char* sdp1 =
      "v=0\r\n"
      "o=saxo 1136992187 1763131365641 IN IP4 127.0.0.1\r\n"
      "s=SDP data\r\n"
      "c=IN IP4 127.0.0.1\r\n"
      "t=0 0\r\n"
      "m=audio 37582 RTP/AVP 8 101\r\n"
      "a=rtpmap:8 PCMA/8000\r\n"
      "a=rtpmap:101 telephone-event/8000\r\n"
      "a=rtcp:37583\r\n"
      "a=sendonly\r\n";

      AmSdp sdp;
      fct_chk(sdp.parse((char *)sdp1));
      fct_chk(sdp.media[0].rtcp_address.getPort() == 37583);

      /* what AmB2BMedia::replaceConnectionAddress() does when we relay media */
      sdp.media[0].port = 18142;
      sdp.media[0].replaceRtcpAddress(18143);

      /* both the advertised and the effective address describe us now */
      fct_chk(sdp.media[0].rtcp_address_orig.getPort() == 18143);
      fct_chk(sdp.media[0].rtcp_address.getPort() == 18143);

      string sdp2;
      sdp.print(sdp2);
      fct_chk(sdp2.find("a=rtcp:18143\r\n") != string::npos);
      fct_chk(sdp2.find("37583") == string::npos);
    } FCT_TEST_END();

  /* an explicitly advertised remote RTCP address must not leak either */
  FCT_TEST_BGN(sdp_replace_rtcp_address_with_conn) {
    const char* sdp1 =
      "v=0\r\n"
      "o=- 0 0 IN IP4 135.125.98.189\r\n"
      "s=-\r\n"
      "c=IN IP4 135.125.98.189\r\n"
      "t=0 0\r\n"
      "m=audio 49346 RTP/AVP 8\r\n"
      "a=rtpmap:8 PCMA/8000\r\n"
      "a=rtcp:49348 IN IP4 10.11.91.45\r\n"
      "a=sendonly\r\n";

      AmSdp sdp;
      fct_chk(sdp.parse((char *)sdp1));

      sdp.media[0].port = 18146;
      sdp.media[0].replaceRtcpAddress(18147);

      string sdp2;
      sdp.print(sdp2);
      /* only our port, the connection address comes from the c= line */
      fct_chk(sdp2.find("a=rtcp:18147\r\n") != string::npos);
      fct_chk(sdp2.find("10.11.91.45") == string::npos);
    } FCT_TEST_END();

  /* our RTCP port is stated even when the remote party advertised none */
  FCT_TEST_BGN(sdp_replace_rtcp_address_added) {
    const char* sdp1 =
      "v=0\r\n"
      "o=- 0 0 IN IP4 192.168.1.20\r\n"
      "s=-\r\n"
      "c=IN IP4 192.168.1.20\r\n"
      "t=0 0\r\n"
      "m=audio 61775 RTP/AVP 8\r\n"
      "a=rtpmap:8 PCMA/8000\r\n"
      "a=sendonly\r\n";

      AmSdp sdp;
      fct_chk(sdp.parse((char *)sdp1));

      sdp.media[0].port = 18142;
      sdp.media[0].replaceRtcpAddress(18143);

      string sdp2;
      sdp.print(sdp2);
      fct_chk(sdp2.find("a=rtcp:18143\r\n") != string::npos);
    } FCT_TEST_END();

  /* with rtcp-mux our RTCP port is the RTP one, and the a=rtcp of the remote
   * party must not survive the replacement either */
  FCT_TEST_BGN(sdp_replace_rtcp_address_muxed) {
    const char* sdp1 =
      "v=0\r\n"
      "o=- 0 0 IN IP4 192.168.1.20\r\n"
      "s=-\r\n"
      "c=IN IP4 192.168.1.20\r\n"
      "t=0 0\r\n"
      "m=audio 38004 RTP/AVP 8\r\n"
      "a=rtpmap:8 PCMA/8000\r\n"
      "a=rtcp:38004\r\n"
      "a=rtcp-mux\r\n"
      "a=sendrecv\r\n";

      AmSdp sdp;
      fct_chk(sdp.parse((char *)sdp1));
      fct_chk(sdp.media[0].hasRtcpMux());

      /* AmRtpSocketPair::getLocalRtcpPort() gives the RTP port when muxed */
      sdp.media[0].port = 18142;
      sdp.media[0].replaceRtcpAddress(18142);

      string sdp2;
      sdp.print(sdp2);
      fct_chk(sdp2.find("a=rtcp:18142\r\n") != string::npos);
      fct_chk(sdp2.find("a=rtcp-mux\r\n") != string::npos);
      fct_chk(sdp2.find("38004") == string::npos);
    } FCT_TEST_END();

} FCTMF_SUITE_END();
