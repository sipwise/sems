#include "tr_blacklist.h"

#include "hash.h"

#define BLACKLIST_HT_POWER 6
#define BLACKLIST_HT_SIZE  (1 << BLACKLIST_HT_POWER)
#define BLACKLIST_HT_MASK  (BLACKLIST_HT_SIZE - 1)

#define DBG_BL INFO

bl_addr::bl_addr()
{
  ss_family = AF_INET;
}

unsigned int bl_addr::hash() const
{
  return hashlittle((sockaddr_storage*)this, SA_len(this), 0)
    & BLACKLIST_HT_MASK;
}

void bl_timer::fire()
{
  DBG_BL("blacklist: %s/%i expired",
	 am_inet_ntop(&addr).c_str(),am_get_port(&addr));
  tr_blacklist::instance()->remove(addr);
}

bool blacklist_bucket::insert(const bl_addr& addr, uint64_t duration /* ms */,
			      const char* reason)
{
  wheeltimer* wt = wheeltimer::instance();
  uint64_t expires = duration * 1000;

  bl_timer* t = new bl_timer(addr);
  bl_entry* bl_e = new bl_entry(t);

  if(!bl_bucket_base::insert(addr,bl_e)) {
    delete t;
    return false;
  }

  DBG_BL("blacklist: added %s/%i (%s/TTL=%.1fs)",
	 am_inet_ntop(&addr).c_str(),am_get_port(&addr),
	 reason,(float)duration/1000.0);

  wt->insert_timer(t, expires);
  return true;
}

bool blacklist_bucket::remove(const bl_addr& addr)
{
  value_map::iterator it = find(addr);

  if(it != elmts.end()){
    bl_entry* v = it->second;
    wheeltimer::instance()->remove_timer(v->t);
    elmts.erase(it);
    delete v;
    return true;
  }

  return false;
}

tr_blacklist::tr_blacklist()
  : blacklist_ht(BLACKLIST_HT_SIZE)
{
}

tr_blacklist::~tr_blacklist()
{ 
}

bool tr_blacklist::exist(const sockaddr_storage& addr)
{
  bool res;
  const bl_addr& bl_a = static_cast<const bl_addr&>(addr);

  blacklist_bucket* bucket = get_bucket(bl_a.hash());
  bucket->lock();
  res = bucket->exist(bl_a);
  bucket->unlock();

  return res;
}

void tr_blacklist::insert(const sockaddr_storage& addr, unsigned int duration,
			   const char* reason)
{
  if(!duration)
    return;

  const bl_addr& bl_a = static_cast<const bl_addr&>(addr);

  blacklist_bucket* bucket = get_bucket(bl_a.hash());
  bucket->lock();
  if(!bucket->exist(bl_a)) {
    bucket->insert(bl_a,duration,reason);
  }
  bucket->unlock();
}

void tr_blacklist::remove(const sockaddr_storage& addr)
{
  const bl_addr& bl_a = static_cast<const bl_addr&>(addr);

  blacklist_bucket* bucket = get_bucket(bl_a.hash());
  bucket->lock();
  bucket->remove(bl_a);
  bucket->unlock();
}
