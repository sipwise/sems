#ifndef _parse_next_hop_h_
#define _parse_next_hop_h_

#include <list>
#include <string>

using std::list;
using std::string;

struct sip_destination
{
  string         host;
  unsigned short port;
  string         trsp;

  sip_destination()
    : host(), port(0), trsp()
  {}
};

int parse_next_hop(const string& next_hop,
		   list<sip_destination>& dest_list);

#endif
