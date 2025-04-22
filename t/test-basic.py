#!/usr/bin/env python3

import sems_tester
import socket


class TestBasic(sems_tester.TestCase):
    _config_base = "basic"
    _sip_port = 5080
    _xmlrpc_port = 8090

    def testOptions(self):
        self.sendRecvSIP(
            b"""OPTIONS sip:monitoring@voip.sipwise.local SIP/2.0
Via: SIP/2.0/UDP 127.0.0.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport
Max-Forwards: 6
To: <sip:monitoring@voip.sipwise.local>
From: monit <sip:monit@127.0.0.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63104 OPTIONS
Contact: <sip:127.0.0.1:57715>
Accept: application/sdp
Content-Length: 0
User-Agent: tester/5.33.0

""",
            b"""^SIP/2.0 200 OK
Via: SIP/2\\.0/UDP 127\\.0\\.0\\.1:57715;branch=z9hG4bKhedcec8e2a445fd80;rport=1;received=[\\d.]+
To: <sip:monitoring@voip.sipwise\\.local>;tag=[A-Z0-9-]+
From: monit <sip:monit@127\\.0\\.0\\.1>;tag=8667fc7c8b14e846
Call-ID: 1eb48ca58e6dbdbb
CSeq: 63104 OPTIONS
Content-Length: 0

$""",
        )

    def testXmlRpc(self):
        self.sendRecvXMLRPC(
            b"""<?xml version="1.0"?>
  <methodCall>
    <methodName>postDSMEvent</methodName>
    <params>
      <param>
        <value><string>sw_audio</string></value>
      </param>
      <param>
        <value><array><data>
          <value><array><data>
            <value><string>cmd</string></value>
            <value><string>clearSets</string></value>
          </data></array></value>
          <value><array><data>
            <value><string>sound_sets</string></value>
            <value><string>1</string></value>
          </data></array></value>
        </data></array></value>
      </param>
    </params>
  </methodCall>""",
            b"""^<\\?xml version="1.0"\\?>
<methodResponse><fault>
	<value><struct><member><name>faultCode</name><value><i4>-1</i4></value></member><member><name>faultString</name><value>postDSMEvent: unknown method name</value></member></struct></value>
</fault></methodResponse>
$""",
        )


if __name__ == "__main__":
    sems_tester.main()
