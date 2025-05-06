#ifndef _RegisterCache_h_
#define _RegisterCache_h_

#include "singleton.h"
#include "hash_table.h"
#include "atomic_types.h"

#include "AmSipMsg.h"
#include "AmUriParser.h"
#include "AmAppTimer.h"

#include <string>
#include <map>
#include <unordered_map>
#include <memory>
#include <set>
using std::string;
using std::map;
using std::unordered_map;
using std::unique_ptr;
using std::set;

#define REG_CACHE_TABLE_POWER   10
#define REG_CACHE_TABLE_ENTRIES (1<<REG_CACHE_TABLE_POWER)

#define DEFAULT_REG_EXPIRES 3600

/*
 * Register cache:
 * ---------------
 * Data model:
 *  - canonical AoR <--1-to-n--> contacts
 *  - alias         <--1-to-1--> contact
 */

struct RegBinding
{
private:
  // Absolute timestamp representing
  // the expiration timer at the 
  // registrar side
  long int reg_expire;

public:
  // unique-id used as contact user toward the registrar
  string alias;

  RegBinding()
    : reg_expire(0)
  {}

  long int get_expire() const {
    return reg_expire;
  }

  friend class AorEntry;
};

template<> struct std::less<unordered_map<string, RegBinding>::iterator>
{
  bool operator()(const unordered_map<string, RegBinding>::iterator& a,
      const unordered_map<string, RegBinding>::iterator& b) const
  {
    if (a->second.get_expire() < b->second.get_expire())
      return true;
    if (a->second.get_expire() > b->second.get_expire())
      return false;
    // for identical expire values we compare the pointer values of the original object
    return &a->second < &b->second;
  }
};

// Contact-URI/Public-IP -> RegBinding
class AorEntry : public unordered_map<string, RegBinding>
{
  // members protected by AorEntry::lock

  set<iterator> bindings_by_time;

  void set_expire(const iterator& it, long int expire) {
    if (it->second.reg_expire == expire)
      return;
    bindings_by_time.erase(it);
    it->second.reg_expire = expire;
    bindings_by_time.insert(it);
  }

  void erase(const iterator& it) {
    bindings_by_time.erase(it);
    unordered_map<string, RegBinding>::erase(it);
  }

  friend class AorHash;

public:
  long int get_lowest_expire() const {
    auto it = bindings_by_time.cbegin();
    if (it == bindings_by_time.cend())
      return LONG_MAX; // empty sets go last
    return (*it)->second.reg_expire;
  }
};

struct AliasEntry
  : public DirectAppTimer
{
  string aor;
  string contact_uri;
  string alias;

  // saved state for NAT handling
  string         source_ip;
  unsigned short source_port;
  string         trsp;

  // sticky interface
  unsigned short local_if;

  // User-Agent
  string remote_ua;

  // Absolute timestamp representing
  // the expiration timer at the 
  // registered UA side
  long int ua_expire;

  AliasEntry()
    : source_port(0), local_if(0), ua_expire(0)
  {}

  // from DirectAppTimer
  void fire();
};

struct RegCacheStorageHandler 
{
  virtual void onDelete(const string& aor, const string& uri, 
			const string& alias) {}

  virtual void onUpdate(const string& canon_aor, const string& alias, 
			long int expires, const AliasEntry& alias_update) {}

  virtual void onUpdate(const string& alias, long int ua_expires) {}
};

/** 
 * Registrar/Reg-Caching 
 * parsing/processing context 
 */
struct RegisterCacheCtx
  : public AmObject
{
  string              from_aor;
  bool               aor_parsed;

  vector<AmUriParser> contacts;
  bool         contacts_parsed;

  unsigned int requested_expires;
  bool            expires_parsed;

  unsigned int min_reg_expires;
  unsigned int max_ua_expires;

  RegisterCacheCtx()
    : aor_parsed(false),
      contacts_parsed(false),
      requested_expires(DEFAULT_REG_EXPIRES),
      expires_parsed(false),
      min_reg_expires(0),
      max_ua_expires(0)
  {}
};

/**
 * Alias hash table:
 *   Alias -> Contact-URI
 */
class AliasHash
  : public unordered_hash_map<string, AliasEntry>
{
public:
  void dump_elmt(const string& alias, const AliasEntry& ae) const;
};

/**
 * AoR hash table:
 *   AoR -> AorEntry
 */
class AorHash
  : public unordered_hash_map<string, AorEntry>
{
public:
  void set_expire(const iterator& aor_it, const AorEntry::iterator& binding_it, long int expire) {
    aor_it->second.set_expire(binding_it, expire);
  }

  void erase_binding(const iterator& aor_it, const AorEntry::iterator& binding_it) {
    aor_it->second.erase(binding_it);
  }

  /* Maintenance stuff */

  void gbc(long int now, list<string>& alias_list);
  void dump_elmt(const string& aor, const AorEntry& p_aor_entry) const;
};

class ContactKey
{
public:
  string uri;
  string ip;
  unsigned short port;

  ContactKey(string _uri, string _ip, unsigned short _port)
    : uri(_uri),
      ip(_ip),
      port(_port)
  {}
};

template<> struct std::hash<ContactKey> {
  size_t operator()(const ContactKey& k) const {
    return std::hash<string>{}(k.uri) ^ std::hash<string>{}(k.ip) ^ std::hash<unsigned int>{}(k.port);
  }
};
template<> struct std::equal_to<ContactKey> {
  size_t operator()(const ContactKey& a, const ContactKey& b) const {
    return a.uri == b.uri
      && a.ip == b.ip
      && a.port == b.port;
  }
};

class ContactHash
  : public unordered_hash_map<ContactKey, string>
{
public:
  using unordered_hash_map<ContactKey, string>::insert;

  void insert(const string& contact_uri, const string& remote_ip,
	      unsigned short remote_port, const string& alias);

  string getAlias(const string& contact_uri, const string& remote_ip,
		  unsigned short remote_port);

  void remove(const string& contact_uri, const string& remote_ip,
	      unsigned short remote_port);

  void dump_elmt(const ContactKey& key, const string& alias) const;
};

class _RegisterCache
  : public AmThread
{
  AorHash               reg_cache_ht;
  AliasHash             id_idx;
  ContactHash           contact_idx;

  unique_ptr<RegCacheStorageHandler> storage_handler;

  // stats
  atomic_int active_regs;

  void gbc();
  void removeAlias(const string& alias, bool generate_event);

  std::mutex shutdown_mutex;
  std::condition_variable sleep_cond;
  bool shutdown_flag;

protected:
  _RegisterCache();
  ~_RegisterCache();

  void dispose() { stop(); }

  /* AmThread interface */
  void run();
  const char *identify() { return "register cache"; }

  void on_stop() {
    std::lock_guard<std::mutex> _l(shutdown_mutex);
    shutdown_flag = true;
    sleep_cond.notify_all();
  }

  int parseAoR(RegisterCacheCtx& ctx, const AmSipRequest& req, msg_logger *logger);
  int parseContacts(RegisterCacheCtx& ctx, const AmSipRequest& req, msg_logger *logger);
  int parseExpires(RegisterCacheCtx& ctx, const AmSipRequest& req, msg_logger *logger);

  void setAliasUATimer(AliasEntry* alias_e);
  void removeAliasUATimer(AliasEntry* alias_e);

public:
  static string canonicalize_aor(const string& aor);
  static string compute_alias_hash(const string& aor, const string& contact_uri,
				   const string& public_ip);

  void setStorageHandler(RegCacheStorageHandler* h) { storage_handler.reset(h); }

  /**
   * Match, retrieve the contact cache entry associated with the URI passed,
   * and return the alias found in the cache entry.
   *
   * Note: this function locks and unlocks the contact cache bucket.
   *
   * aor: canonical Address-of-Record
   * uri: Contact-URI
   */
  bool getAlias(const string& aor, const string& uri,
		const string& public_ip, RegBinding& out_binding);

  /**
   * Update contact cache entry and alias map entries.
   *
   * Note: this function locks and unlocks 
   *       the contact cache bucket and
   *       the alias map bucket.
   *
   * aor: canonical Address-of-Record
   * uri: Contact-URI
   * alias: 
   */
  void update(const string& alias, long int reg_expires,
	      const AliasEntry& alias_update);

  void update(long int reg_expires, const AliasEntry& alias_update);

  bool updateAliasExpires(const string& alias, long int ua_expires);

  /**
   * Remove contact cache entry and alias map entries.
   *
   * Note: this function locks and unlocks 
   *       the contact cache bucket and
   *       the alias map bucket.
   *
   * aor: canonical Address-of-Record
   * uri: Contact-URI
   * alias:
   */
  void remove(const string& aor, const string& uri,
	      const string& alias);

  void remove(const string& aor);

  /**
   * Retrieve an alias map containing all entries related
   * to a particular AOR. This is needed to support REGISTER
   * with '*' contact.
   *
   * Note: this function locks and unlocks 
   *       the contact cache bucket.
   *
   * aor: canonical Address-of-Record
   * alias_map: alias -> contact
   */
  bool getAorAliasMap(const string& aor, map<string,string>& alias_map);

  /**
   * Retrieve the alias entry related to the given alias
   */
  bool findAliasEntry(const string& alias, AliasEntry& alias_entry);

  /**
   * Retrieve the alias entry related to the given contact-URI, remote-IP & port
   */
  bool findAEByContact(const string& contact_uri, const string& remote_ip,
		       unsigned short remote_port, AliasEntry& ae);

  /**
   * Throttle REGISTER requests
   *
   * Returns false if REGISTER should be forwarded:
   * - if registrar binding should be renewed.
   * - if source IP or port do not match the saved IP & port.
   * - if the request unregisters any contact.
   * - if request is not a REGISTER
   */
  bool throttleRegister(RegisterCacheCtx& ctx,
			const AmSipRequest& req,
                        msg_logger *logger = NULL);

  /**
   * Save a single REGISTER contact into cache
   *
   * Returns false if failed:
   * - if request is not a REGISTER.
   * - more than one contact should be (un)registered.
   *
   * If true has been returned, the request has already 
   * been replied with either an error or 200 (w/ contact).
   *
   * Note: this function also handles binding query.
   *       (REGISTER w/o contacts)
   */
  bool saveSingleContact(RegisterCacheCtx& ctx,
			const AmSipRequest& req,
                        msg_logger *logger = NULL);

  /**
   * Statistics
   */
  unsigned int getActiveRegs() { return active_regs; }
};

typedef singleton<_RegisterCache> RegisterCache;

#endif
