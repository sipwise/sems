#ifndef _sip_parser_async_h_
#define _sip_parser_async_h_

#include <sys/types.h>

#include "parse_header.h"

struct parser_state
{
  char* orig_buf;
  char* c; // cursor
  char* beg; // last marker for field start

  int stage;
  int st; // parser state (within stage)
  int saved_st; // saved parser state (within stage)
  sip_header hdr; // temporary header struct
  
  ssize_t content_len; // detected body content-length

  parser_state()
    : orig_buf(NULL),c(NULL),beg(NULL),
      stage(0),st(0),saved_st(0),
      content_len(0)
  {}

  void reset(char* buf) {
    c = orig_buf = buf;
    reset_hdr_parser();
    stage = content_len = 0;
  }

  void reset_hdr_parser() {
    hdr = {};
    st = saved_st = 0;
    beg = c;
  }

  ssize_t get_msg_len() {
    return c - orig_buf + content_len;
  }
};

int skip_sip_msg_async(parser_state* pst, char* end);

#endif
