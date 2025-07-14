#ifndef _tr_blacklist_h_
#define _tr_blacklist_h_

#include "hash_table.h"
#include "singleton.h"

#include "ip_util.h"
#include "wheeltimer.h"

#include <string.h>

/**
 * Blacklist bucket: key type
 */
struct bl_addr: public sockaddr_storage
{
  bl_addr();
  bl_addr(const bl_addr&);
  bl_addr(const sockaddr_storage*);

  unsigned int hash();
};

template<> struct std::less<bl_addr> {
  bool operator() (const bl_addr& l, const bl_addr& r) const
  {
    if(l.ss_family != r.ss_family) {
      return l.ss_family < r.ss_family;
    }

    struct sockaddr_in* l_v4 = (struct sockaddr_in*)&l;
    struct sockaddr_in* r_v4 = (struct sockaddr_in*)&r;

    struct sockaddr_in6* l_v6 = (struct sockaddr_in6*)&l;
    struct sockaddr_in6* r_v6 = (struct sockaddr_in6*)&r;

    if(l.ss_family == AF_INET) {
      if(l_v4->sin_addr.s_addr != r_v4->sin_addr.s_addr) {
	return l_v4->sin_addr.s_addr < r_v4->sin_addr.s_addr;
      }
      return l_v4->sin_port < r_v4->sin_port;
    }

    int ret = memcmp((void*)&l_v6->sin6_addr,
		     (void*)&r_v6->sin6_addr,
		     sizeof(struct in6_addr));

    if(ret != 0) {
      return ret < 0;
    }

    return l_v6->sin6_port < r_v6->sin6_port;
  }
};

struct bl_entry;

typedef ht_map_bucket<bl_addr,bl_entry> bl_bucket_base;

class blacklist_bucket
  : public bl_bucket_base
{
protected:
  bool insert(const bl_addr& k, bl_entry* v) {
    return bl_bucket_base::insert(k,v);
  }

public:
  blacklist_bucket(unsigned long id)
  : bl_bucket_base(id)
  {}

  bool insert(const bl_addr& addr, uint64_t duration /* ms */,
	      const char* reason);
  bool remove(const bl_addr& addr);
};

typedef blacklist_bucket::value_map::iterator blacklist_elmt;

struct bl_timer
  : public timer
{
  bl_addr addr;

  bl_timer()
    : timer(), addr()
  {}

  bl_timer(const bl_addr& addr)
    : addr(addr)
  {}

  void fire();
};

/**
 * Blacklist bucket: value type
 */
struct bl_entry
{
  bl_timer* t;

  bl_entry() {}

  bl_entry(bl_timer* t)
    : t(t)
  {}
};

typedef hash_table<blacklist_bucket> blacklist_ht;

class tr_blacklist
  : protected blacklist_ht,
    public singleton<tr_blacklist>
{
  friend class singleton<tr_blacklist>;

protected:
  tr_blacklist();
  ~tr_blacklist();

public:
  // public blacklist API:
  bool exist(const sockaddr_storage* addr);
  void insert(const sockaddr_storage* addr, unsigned int duration /* ms */,
	      const char* reason);
  void remove(const sockaddr_storage* addr);
};

#endif
