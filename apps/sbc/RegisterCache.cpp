#include "RegisterCache.h"
#include "sip/hash.h"
#include "sip/parse_uri.h"

#include "AmBasicSipDialog.h"
#include "AmSipHeaders.h"
#include "AmUriParser.h"
#include "RegisterDialog.h"
#include "AmSession.h" //getNewId
#include "AmUtils.h"
#include "SBCEventLog.h"

#include <utility>
#include <mutex>
using std::pair;
using std::make_pair;
using std::lock_guard;

#define REG_CACHE_CYCLE 10L /* 10 seconds to expire all buckets */

static string unescape_sip(const string& str)
{
  // TODO
  return str;
}

void AorHash::dump_elmt(const string& aor,
			  const AorEntry& p_aor_entry) const
{
  DBG("'%s' ->", aor.c_str());

  for (AorEntry::const_iterator it = p_aor_entry.begin();
      it != p_aor_entry.end(); it++) {

    const RegBinding& b = it->second;
    DBG("\t'%s' -> '%s'", it->first.c_str(),
	b.alias.c_str());
  }
}

void AorHash::gbc(long int now,
		    list<string>& alias_list)
{
  for(auto it = begin(); it != end();) {
    AorEntry& aor_e = it->second;

    for (AorEntry::iterator reg_it = aor_e.begin();
	reg_it != aor_e.end();) {

      auto& binding = reg_it->second;

      if (binding.reg_expire <= now) {

	alias_list.push_back(binding.alias);
	AorEntry::iterator del_it = reg_it++;

	DBG("delete binding: '%s' -> '%s' (%li <= %li)",
	    del_it->first.c_str(), binding.alias.c_str(),
	    binding.reg_expire, now);

	it->second.erase(del_it);
	continue;
      }
      reg_it++;
    }
    if(it->second.empty()) {
      DBG("delete empty AOR: '%s'", it->first.c_str());
      auto del_it = it++;
      erase(del_it);
      continue;
    }
    it++;
  }
}

void AliasEntry::fire()
{
  AmArg ev;
  ev["aor"]      = aor;
  ev["to"]       = aor;
  ev["contact"]  = contact_uri;
  ev["source"]   = source_ip;
  ev["src_port"] = source_port;
  ev["from-ua"]  = remote_ua;

  DBG("Alias expired (UA/%li): '%s' -> '%s'\n",
      (long)(AmAppTimer::instance()->unix_clock.get() - ua_expire),
      alias.c_str(),aor.c_str());

  SBCEventLog::instance()->logEvent(alias,"ua-reg-expired",ev);
}

void AliasHash::dump_elmt(const string& alias, const AliasEntry& p_ae) const
{
  DBG("'%s' -> '%s'", alias.c_str(),
      p_ae.contact_uri.c_str());
}

string ContactHash::getAlias(const string& contact_uri,
			       const string& remote_ip,
			       unsigned short remote_port)
{
  auto it = find(ContactKey(contact_uri, remote_ip, remote_port));
  if(it == end())
    return string();

  return it->second;
}

void ContactHash::remove(const string& contact_uri, const string& remote_ip,
			   unsigned short remote_port)
{
  erase(ContactKey(contact_uri, remote_ip, remote_port));
}

void ContactHash::dump_elmt(const ContactKey& key, const string& alias) const
{
  DBG("'%s/%s:%u' -> %s", key.uri.c_str(), key.ip.c_str(), key.port, alias.c_str());
}

struct RegCacheLogHandler
  : RegCacheStorageHandler
{
  void onDelete(const string& aor, const string& uri, const string& alias) {
    DBG("delete: aor='%s';uri='%s';alias='%s'",
	aor.c_str(),uri.c_str(),alias.c_str());
  }

  void onUpdate(const string& canon_aor, const string& alias, 
		long int expires, const AliasEntry& alias_update) {
    DBG("update: aor='%s';alias='%s';expires=%li",
	canon_aor.c_str(),alias.c_str(),expires);
  }

  void onUpdate(const string& alias, long int ua_expires) {
    DBG("update: alias='%s';ua_expires=%li",
	alias.c_str(),ua_expires);
  }
};


_RegisterCache::_RegisterCache()
  : shutdown_flag(false)
{
  // debug register cache WRITE operations
  setStorageHandler(new RegCacheLogHandler());
}

_RegisterCache::~_RegisterCache()
{
  DBG("##### REG CACHE DUMP #####");
  reg_cache_ht.dump();
  DBG("##### ID IDX DUMP #####");
  id_idx.dump();
  DBG("##### CONTACT IDX DUMP #####");
  contact_idx.dump();
  DBG("##### DUMP END #####");
}

void _RegisterCache::gbc()
{
  struct timeval now;
  gettimeofday(&now,NULL);

  lock_guard<AmMutex> _rl(reg_cache_ht);
  list<string> alias_list;
  reg_cache_ht.gbc(now.tv_sec, alias_list);
  for(list<string>::iterator it = alias_list.begin();
      it != alias_list.end(); it++){
    removeAlias(*it,true);
  }
}

void _RegisterCache::run()
{
  while (!stop_requested()) {
    gbc();

    std::unique_lock<std::mutex> _l(shutdown_mutex);
    if (shutdown_flag)
      break;
    sleep_cond.wait_for(_l, std::chrono::seconds(REG_CACHE_CYCLE));
  }
}

/**
 * From RFC 3261 (Section 10.3, step 5):
 *  "all URI parameters MUST be removed (including the user-param), and
 *   any escaped characters MUST be converted to their unescaped form"
 */
string _RegisterCache::canonicalize_aor(const string& uri)
{
  string canon_uri;
  sip_uri parsed_uri;

  if(parse_uri(&parsed_uri,uri.c_str(),uri.length())) {
    DBG("Malformed URI: '%s'",uri.c_str());
    return "";
  }

  switch(parsed_uri.scheme) {
  case sip_uri::SIP:  canon_uri = "sip:"; break;
  case sip_uri::SIPS: canon_uri = "sips:"; break;
  default:
    DBG("Unknown URI scheme in '%s'",uri.c_str());
    return "";
  }

  if(parsed_uri.user.len) {
    canon_uri += unescape_sip(c2stlstr(parsed_uri.user)) + "@";
  }

  canon_uri += unescape_sip(c2stlstr(parsed_uri.host));

  if(parsed_uri.port != 5060) {
    canon_uri += ":" + unescape_sip(c2stlstr(parsed_uri.port_str));
  }

  return canon_uri;
}

string 
_RegisterCache::compute_alias_hash(const string& aor, const string& contact_uri,
				   const string& public_ip)
{
  unsigned int h1=0,h2=0;
  h1 = hashlittle(aor.c_str(),aor.length(),h1);
  h1 = hashlittle(contact_uri.c_str(),contact_uri.length(),h1);
  h2 = hashlittle(public_ip.c_str(),public_ip.length(),h1);

  return int2hex(h1,true) + int2hex(h2,true);
}

void ContactHash::insert(const string& contact_uri, const string& remote_ip,
			   unsigned short remote_port, const string& alias)
{
  insert(make_pair(ContactKey(contact_uri, remote_ip, remote_port), alias));
}

bool _RegisterCache::getAlias(const string& canon_aor, const string& uri,
			      const string& public_ip, RegBinding& out_binding)
{
  if(canon_aor.empty()) {
    DBG("Canonical AOR is empty");
    return false;
  }

  bool alias_found = false;
  lock_guard<AmMutex> _rl(reg_cache_ht);

  auto aor_e_it = reg_cache_ht.find(canon_aor);
  if (aor_e_it != reg_cache_ht.end()) {
    auto& aor_e = aor_e_it->second;
    AorEntry::iterator binding_it = aor_e.find(uri + "/" + public_ip);
    if ((binding_it != aor_e.end())) {
      alias_found = true;
      out_binding = binding_it->second;
    }
  }

  return alias_found;
}

void _RegisterCache::setAliasUATimer(AliasEntry* alias_e)
{
  if(!alias_e->ua_expire)
    return;

  AmAppTimer* app_timer = AmAppTimer::instance();
  time_t timeout = alias_e->ua_expire - app_timer->unix_clock.get();
  if(timeout > 0) {
    app_timer->setTimer(alias_e,timeout);
  }
  else {
    // already expired at the UA side, just fire the timer handler
    alias_e->fire();
  }
}

void _RegisterCache::removeAliasUATimer(AliasEntry* alias_e)
{
  AmAppTimer::instance()->removeTimer(alias_e);
}

void _RegisterCache::update(const string& alias, long int reg_expires,
			    const AliasEntry& alias_update)
{
  string uri = alias_update.contact_uri;
  string canon_aor = alias_update.aor;
  string public_ip = alias_update.source_ip;
  if(canon_aor.empty()) {
    ERROR("Canonical AOR is empty: could not update register cache");
    return;
  }
  if(uri.empty()) {
    ERROR("Contact-URI is empty: could not update register cache");
    return;
  }
  if(public_ip.empty()) {
    ERROR("Source-IP is empty: could not update register cache");
    return;
  }

  lock_guard<AmMutex> _rl(reg_cache_ht);
  lock_guard<AmMutex> _id_l(id_idx);

  // Try to get the existing binding
  AorEntry::iterator binding_it;
  auto aor_e_it = reg_cache_ht.find(canon_aor);
  if (aor_e_it == reg_cache_ht.end()) {
    // insert AorEntry if none
    aor_e_it = reg_cache_ht.insert(make_pair(canon_aor, AorEntry())).first;
    DBG("inserted new AOR '%s'",canon_aor.c_str());
    binding_it = aor_e_it->second.end();
  }
  else {
    string idx = uri + "/" + public_ip;
    binding_it = aor_e_it->second.find(idx);
  }

  if (binding_it == aor_e_it->second.end()) {
    // insert one if none exist
    binding_it = aor_e_it->second.insert(make_pair(uri + "/" + public_ip, RegBinding())).first;
    binding_it->second.alias = alias;
    DBG("inserted new binding: '%s' -> '%s'",
	uri.c_str(), alias.c_str());

    // inc stats
    active_regs++;

    lock_guard<AmMutex> _cl(contact_idx);
    contact_idx.insert(uri, alias_update.source_ip,
		      alias_update.source_port, alias);
  }
  else {
    DBG("updating existing binding: '%s' -> '%s'",
	uri.c_str(), binding_it->second.alias.c_str());
    if(alias != binding_it->second.alias) {
      ERROR("used alias ('%s') is different from stored one ('%s')",
	    alias.c_str(), binding_it->second.alias.c_str());
    }
  }
  // and update binding
  binding_it->second.reg_expire = reg_expires;

  auto alias_e_it = id_idx.find(alias);
  // if no alias map entry, insert a new one
  if (alias_e_it == id_idx.end()) {
    DBG("inserting alias map entry: '%s' -> '%s'",
	alias.c_str(), uri.c_str());
    alias_e_it = id_idx.insert(make_pair(alias, AliasEntry(alias_update))).first;
  }
  else {
    alias_e_it->second = alias_update;
  }

#if 0 // disabled UA-timer
  if(alias_e->ua_expire) {
    setAliasUATimer(alias_e);
  }
#endif
  
  if(storage_handler.get())
    storage_handler->onUpdate(canon_aor,alias,reg_expires, alias_e_it->second);
}

void _RegisterCache::update(long int reg_expires, const AliasEntry& alias_update)
{
  string uri = alias_update.contact_uri;
  string canon_aor = alias_update.aor;
  string public_ip = alias_update.source_ip;
  if(canon_aor.empty()) {
    ERROR("Canonical AOR is empty: could not update register cache");
    return;
  }
  if(uri.empty()) {
    ERROR("Contact-URI is empty: could not update register cache");
    return;
  }
  if(public_ip.empty()) {
    ERROR("Source-IP is empty: could not update register cache");
    return;
  }

  string idx = uri + "/" + public_ip;
  lock_guard<AmMutex> _rl(reg_cache_ht);

  // Try to get the existing binding
  AorEntry::iterator binding_it;
  auto aor_e_it = reg_cache_ht.find(canon_aor);
  if (aor_e_it != reg_cache_ht.end()) {
    // take the first, as we do not expect others to be here
    binding_it = aor_e_it->second.begin();

    if  (binding_it != aor_e_it->second.end()) {

      if ((binding_it->first != idx)) {

	// contact-uri and/or public IP has changed...
	string alias = binding_it->second.alias;

	AliasEntry ae;
	if(findAliasEntry(alias,ae)) {

	  // change contact index
	  lock_guard<AmMutex> _cl(contact_idx);
	  contact_idx.remove(ae.contact_uri, ae.source_ip, ae.source_port);
	  contact_idx.insert(uri, public_ip, alias_update.source_port, alias);
	}

	// relink binding with the new index
	auto binding = binding_it->second;
	aor_e_it->second.erase(binding_it);
	binding_it = aor_e_it->second.insert(make_pair(idx, binding)).first;
      }
    }
  } 
  else {
    // insert AorEntry if none
    aor_e_it = reg_cache_ht.insert(make_pair(canon_aor, AorEntry())).first;
    DBG("inserted new AOR '%s'",canon_aor.c_str());
    binding_it = aor_e_it->second.end();
  }
  
  if (binding_it == aor_e_it->second.end()) {
    // insert one if none exist
    RegBinding binding;
    binding.alias = _RegisterCache::
      compute_alias_hash(canon_aor,uri,public_ip);

    // inc stats
    active_regs++;

    string idx = uri + "/" + public_ip;
    binding_it = aor_e_it->second.insert(make_pair(idx, std::move(binding))).first;
    DBG("inserted new binding: '%s' -> '%s'",
	idx.c_str(), binding_it->second.alias.c_str());

    lock_guard<AmMutex> _cl(contact_idx);
    contact_idx.insert(uri, alias_update.source_ip,
		      alias_update.source_port, binding_it->second.alias);
  }
  else {
    DBG("updating existing binding: '%s' -> '%s'",
	uri.c_str(), binding_it->second.alias.c_str());
  }
  // and update binding
  binding_it->second.reg_expire = reg_expires;

  lock_guard<AmMutex> _id_l(id_idx);

  auto alias_e_it = id_idx.find(binding_it->second.alias);
  // if no alias map entry, insert a new one
  if (alias_e_it == id_idx.end()) {
    DBG("inserting alias map entry: '%s' -> '%s'",
	binding_it->second.alias.c_str(), uri.c_str());
    alias_e_it = id_idx.insert(make_pair(binding_it->second.alias, AliasEntry(alias_update))).first;
  }
  else
    alias_e_it->second = alias_update;

  alias_e_it->second.alias = binding_it->second.alias;

#if 0 // disabled UA-timer
  if(alias_e->ua_expire) {
    setAliasUATimer(alias_e);
  }
#endif
  
  if(storage_handler.get())
    storage_handler->onUpdate(canon_aor, binding_it->second.alias,
			      reg_expires, alias_e_it->second);
}

bool _RegisterCache::updateAliasExpires(const string& alias, long int ua_expires)
{
  bool res = false;
  lock_guard<AmMutex> _id_l(id_idx);

  auto alias_e_it = id_idx.find(alias);
  if (alias_e_it != id_idx.end()) {
    alias_e_it->second.ua_expire = ua_expires;
#if 0 // disabled UA-timer
    if(alias_e->ua_expire)
      setAliasUATimer(alias_e);
#endif

    if(storage_handler.get()) {
      storage_handler->onUpdate(alias,ua_expires);
    }
    res = true;
  }

  return res;
}

void _RegisterCache::remove(const string& canon_aor, const string& uri,
			    const string& alias)
{
  if(canon_aor.empty()) {
    DBG("Canonical AOR is empty");
    return;
  }

  lock_guard<AmMutex> _rl(reg_cache_ht);

  DBG("removing entries for aor = '%s', uri = '%s' and alias = '%s'",
      canon_aor.c_str(), uri.c_str(), alias.c_str());

  auto aor_e_it = reg_cache_ht.find(canon_aor);
  if (aor_e_it != reg_cache_ht.end()) {
    auto& aor_e = aor_e_it->second;
    // remove all bindings for which the alias matches
    for(AorEntry::iterator binding_it = aor_e.begin();
	binding_it != aor_e.end();) {

      auto& binding = binding_it->second;
      if (binding.alias == alias) {

	AorEntry::iterator del_it = binding_it++;
	aor_e.erase(del_it);
	continue;
      }

      binding_it++;
    }
    if(aor_e.empty()) {
      reg_cache_ht.erase(aor_e_it);
    }
  }

  removeAlias(alias,false);
}

void _RegisterCache::remove(const string& aor)
{
  if(aor.empty()) {
    DBG("Canonical AOR is empty");
    return;
  }

  lock_guard<AmMutex> _rl(reg_cache_ht);

  DBG("removing entries for aor = '%s'", aor.c_str());

  auto aor_e_it = reg_cache_ht.find(aor);
  if (aor_e_it != reg_cache_ht.end()) {
    auto& aor_e = aor_e_it->second;
    for(AorEntry::iterator binding_it = aor_e.begin();
	binding_it != aor_e.end(); binding_it++) {

      auto& binding = binding_it->second;
      removeAlias(binding.alias, false);
    }
    reg_cache_ht.erase(aor_e_it);
  }
}

void _RegisterCache::removeAlias(const string& alias, bool generate_event)
{
  lock_guard<AmMutex> _id_l(id_idx);

  auto ae_it = id_idx.find(alias);
  if (ae_it != id_idx.end()) {
#if 0 // disabled UA-timer
    if(ae->ua_expire)
      removeAliasUATimer(ae);
#endif
    auto& ae = ae_it->second;

    if(generate_event) {
      AmArg ev;
      ev["aor"]      = ae.aor;
      ev["to"]       = ae.aor;
      ev["contact"]  = ae.contact_uri;
      ev["source"]   = ae.source_ip;
      ev["src_port"] = ae.source_port;
      ev["from-ua"]  = ae.remote_ua;
    
      DBG("Alias expired @registrar (UA/%li): '%s' -> '%s'\n",
	  (long)(AmAppTimer::instance()->unix_clock.get() - ae.ua_expire),
	  ae.alias.c_str(), ae.aor.c_str());

      SBCEventLog::instance()->logEvent(ae.alias, "reg-expired", ev);
    }

    {
      lock_guard<AmMutex> _cl(contact_idx);
      contact_idx.remove(ae.contact_uri, ae.source_ip, ae.source_port);
    }

    // dec stats
    active_regs--;

    storage_handler->onDelete(ae.aor,
			      ae.contact_uri,
			      ae.alias);

    id_idx.erase(ae_it);
  }
}

bool _RegisterCache::getAorAliasMap(const string& canon_aor, 
				    map<string,string>& alias_map)
{
  if(canon_aor.empty()) {
    DBG("Canonical AOR is empty");
    return false;
  }

  lock_guard<AmMutex> _rl(reg_cache_ht);
  auto aor_e_it = reg_cache_ht.find(canon_aor);
  if (aor_e_it != reg_cache_ht.end()) {
    for (AorEntry::iterator it = aor_e_it->second.begin();
	it != aor_e_it->second.end(); ++it) {

      AliasEntry ae;
      if(!findAliasEntry(it->second.alias,ae))
	continue;

      alias_map[ae.alias] = ae.contact_uri;
    }
  }

  return true;
}

bool _RegisterCache::findAliasEntry(const string& alias, AliasEntry& alias_entry)
{
  bool res = false;

  lock_guard<AmMutex> _id_l(id_idx);
  
  auto it = id_idx.find(alias);
  if (it != id_idx.end()) {
    alias_entry = it->second;
    res = true;
  }

  return res;
}

bool _RegisterCache::findAEByContact(const string& contact_uri,
				     const string& remote_ip,
				     unsigned short remote_port,
				     AliasEntry& ae)
{
  bool res = false;

  string alias;
  {
    lock_guard<AmMutex> _cl(contact_idx);
    alias = contact_idx.getAlias(contact_uri, remote_ip, remote_port);
  }

  if(alias.empty())
    return false;

  res = findAliasEntry(alias,ae);

  return res;
}


int _RegisterCache::parseAoR(RegisterCacheCtx& ctx,
			     const AmSipRequest& req,
                             msg_logger *logger)
{
  if(ctx.aor_parsed)
    return 0;

  AmUriParser from_parser;
  size_t end_from = 0;
  if(!from_parser.parse_contact(req.from,0,end_from)) {
    DBG("error parsing AoR: '%s'\n",req.from.c_str());
    AmBasicSipDialog::reply_error(req,400,"Bad request - bad From HF", "", logger);
    return -1;
  }

  ctx.from_aor = RegisterCache::canonicalize_aor(from_parser.uri_str());
  DBG("parsed AOR: '%s'",ctx.from_aor.c_str());

  if(ctx.from_aor.empty()) {
    AmBasicSipDialog::reply_error(req,400,"Bad request - bad From HF", "", logger);
    return -1;
  }
  ctx.aor_parsed = true;

  return 0;
}

int _RegisterCache::parseContacts(RegisterCacheCtx& ctx,
				  const AmSipRequest& req,
                                  msg_logger *logger)
{
  if(ctx.contacts_parsed)
    return 0;

  if ((RegisterDialog::parseContacts(req.contact, ctx.contacts) < 0) ||
      (ctx.contacts.size() == 0)) {
    AmBasicSipDialog::reply_error(req, 400, "Bad Request", 
				  "Warning: Malformed contact\r\n", logger);
    return -1;
  }
  ctx.contacts_parsed = true;
  return 0;
}

int _RegisterCache::parseExpires(RegisterCacheCtx& ctx,
				 const AmSipRequest& req,
                                 msg_logger *logger)
{
  if(ctx.expires_parsed)
    return 0;

  // move Expires as separate header to contact parameter
  string expires_str = getHeader(req.hdrs, "Expires");
  if (!expires_str.empty() && str2int(expires_str, ctx.requested_expires)) {
    AmBasicSipDialog::reply_error(req, 400, "Bad Request", 
				  "Warning: Malformed expires\r\n", logger);
    return true; // error reply sent
  }
  ctx.expires_parsed = true;
  return 0;
}

bool _RegisterCache::throttleRegister(RegisterCacheCtx& ctx,
				      const AmSipRequest& req,
                                      msg_logger *logger)
{
  if (req.method != SIP_METH_REGISTER) {
    ERROR("unsupported method '%s'\n", req.method.c_str());
    return false; // fwd
  }

  if (req.contact.empty() || (req.contact == "*")) {
    // binding query or unregister
    DBG("req.contact.empty() || (req.contact == \"*\")\n");
    return false; // fwd
  }

  if ((parseAoR(ctx,req, logger) < 0) ||
      (parseContacts(ctx,req, logger) < 0) ||
      (parseExpires(ctx,req, logger) < 0)) {
    DBG("could not parse AoR, Contact or Expires\n");
    return true; // error reply sent
  }

  unsigned int default_expires;
  if(ctx.requested_expires && (ctx.requested_expires > ctx.max_ua_expires))
    default_expires = ctx.max_ua_expires;
  else
    default_expires = ctx.requested_expires;

  vector<pair<string, long int> > alias_updates;
  for(vector<AmUriParser>::iterator contact_it = ctx.contacts.begin();
      contact_it != ctx.contacts.end(); contact_it++) {

    map<string, string>::iterator expires_it = 
      contact_it->params.find("expires");

    long int contact_expires=0;
    if(expires_it == contact_it->params.end()) {
      if(!default_expires){
	DBG("!default_expires");
	return false; // fwd
      }

      contact_expires = default_expires;
      contact_it->params["expires"] = long2str(contact_expires);
    }
    else {
      if(!str2int(expires_it->second,contact_expires)) {
	AmBasicSipDialog::reply_error(req, 400, "Bad Request",
				      "Warning: Malformed expires\r\n", logger);
	return true; // error reply sent
      }

      if(!contact_expires) {
	DBG("!contact_expires");
	return false; // fwd
      }

      if(contact_expires && ctx.max_ua_expires &&
	 (contact_expires > (long int)ctx.max_ua_expires)) {

	contact_expires = ctx.max_ua_expires;
	contact_it->params["expires"] = long2str(contact_expires);
      }
    }

    RegBinding reg_binding;
    const string& uri = contact_it->uri_str();

    if(!getAlias(ctx.from_aor,uri,req.remote_ip,reg_binding) ||
       !reg_binding.reg_expire) {
      DBG("!getAlias(%s,%s,...) || !reg_binding.reg_expire",
	  ctx.from_aor.c_str(),uri.c_str());
      return false; // fwd
    }

    struct timeval now;
    gettimeofday(&now,NULL);
    contact_expires += now.tv_sec;

    if(contact_expires + 4 /* 4 seconds buffer */ 
       >= reg_binding.reg_expire) {
      DBG("%li + 4 >= %li",contact_expires,reg_binding.reg_expire);
      return false; // fwd
    }
    
    AliasEntry alias_entry;
    if(!findAliasEntry(reg_binding.alias, alias_entry) ||
       (alias_entry.source_ip != req.remote_ip) ||
       (alias_entry.source_port != req.remote_port)) {
      DBG("no alias entry or IP/port mismatch");
      return false; // fwd
    }

    alias_updates.push_back(make_pair(reg_binding.alias,
						       contact_expires));
  }

  // reply 200 w/ contacts
  vector<AmUriParser>::iterator it = ctx.contacts.begin();
  vector<pair<string, long int> >::iterator alias_update_it = 
    alias_updates.begin();

  string contact_hdr = SIP_HDR_COLSP(SIP_HDR_CONTACT) + it->print();
  assert(alias_update_it != alias_updates.end());
  if(!updateAliasExpires(alias_update_it->first,
			 alias_update_it->second)) {
    // alias not found ???
    return false; // fwd
  }
  it++;
  alias_update_it++;

  for(;it != ctx.contacts.end(); it++, alias_update_it++) {

    contact_hdr += ", " + it->print();

    assert(alias_update_it != alias_updates.end());
    if(!updateAliasExpires(alias_update_it->first,
			   alias_update_it->second)) {
      // alias not found ???
      return false; // fwd
    }
  }
  contact_hdr += CRLF;

  // send 200 reply
  AmBasicSipDialog::reply_error(req, 200, "OK", contact_hdr, logger);
  return true;
}

bool _RegisterCache::saveSingleContact(RegisterCacheCtx& ctx,
				       const AmSipRequest& req,
                                       msg_logger *logger)
{
  if (req.method != SIP_METH_REGISTER) {
    ERROR("unsupported method '%s'\n", req.method.c_str());
    return false;
  }

  if(parseAoR(ctx,req, logger) < 0) {
    return true;
  }

  if (req.contact.empty()) {
    string contact_hdr;
    map<string,string> alias_map;
    if(getAorAliasMap(ctx.from_aor, alias_map) &&
       !alias_map.empty()) {

      struct timeval now;
      gettimeofday(&now,NULL);

      AliasEntry alias_entry;
      if(findAliasEntry(alias_map.begin()->first,alias_entry) &&
	 (now.tv_sec < alias_entry.ua_expire)) {

	unsigned int exp = alias_entry.ua_expire - now.tv_sec;
	contact_hdr = SIP_HDR_COLSP(SIP_HDR_CONTACT)
	  + alias_entry.contact_uri + ";expires=" 
	  + int2str(exp) + CRLF;
      }
    }
    
    AmBasicSipDialog::reply_error(req, 200, "OK", contact_hdr);
    return true;
  }

  bool star_contact=false;
  unsigned int contact_expires=0;
  AmUriParser* contact=NULL;
  if (req.contact == "*") {
    // unregister everything
    star_contact = true;

    if(parseExpires(ctx,req, logger) < 0) {
      return true;
    }

    if(ctx.requested_expires != 0) {
      AmBasicSipDialog::reply_error(req, 400, "Bad Request",
				    "Warning: Expires not equal 0\r\n");
      return true;
    }
  }
  else if ((parseContacts(ctx,req, logger) < 0) ||
	   (parseExpires(ctx,req, logger) < 0)) {
    return true; // error reply sent
  }
  else if (ctx.contacts.size() != 1) {
    AmBasicSipDialog::reply_error(req, 403, "Forbidden",
				  "Warning: only one contact allowed\r\n", logger);
    return true; // error reply sent
  }
  else {
    
    contact = &ctx.contacts[0];
    if(contact->params.find("expires") != contact->params.end()) {
      DBG("contact->params[\"expires\"] = '%s'",
	  contact->params["expires"].c_str());
      if(str2int(contact->params["expires"],contact_expires)) {
	AmBasicSipDialog::reply_error(req, 400, "Bad Request",
				      "Warning: Malformed expires\r\n", logger);
	return true; // error reply sent
      }
      DBG("contact_expires = %u",contact_expires);
    }
    else {
      contact_expires = ctx.requested_expires;
    }
  }

  if(!contact_expires) {
    // unregister AoR
    remove(ctx.from_aor);
    AmBasicSipDialog::reply_error(req, 200, "OK", "", logger);
    return true;
  }
  assert(contact);  

  // throttle contact_expires
  unsigned int reg_expires = contact_expires;
  if(reg_expires && (reg_expires < ctx.min_reg_expires))
    reg_expires = ctx.min_reg_expires;
  
  unsigned int ua_expires = contact_expires;
  if(ua_expires && ctx.max_ua_expires && 
     (ua_expires > ctx.max_ua_expires))
    ua_expires = ctx.max_ua_expires;

  struct timeval now;
  gettimeofday(&now,NULL);

  reg_expires += now.tv_sec;

  AliasEntry alias_update;
  alias_update.aor = ctx.from_aor;
  alias_update.contact_uri = contact->uri_str();
  alias_update.source_ip = req.remote_ip;
  alias_update.source_port = req.remote_port;
  alias_update.remote_ua = getHeader(req.hdrs,"User-Agent");
  alias_update.trsp = req.trsp;
  alias_update.local_if = req.local_if;
  alias_update.ua_expire = ua_expires + now.tv_sec;

  update(reg_expires,alias_update);

  contact->params["expires"] = int2str(ua_expires);
  string contact_hdr = SIP_HDR_COLSP(SIP_HDR_CONTACT)
    + contact->print() + CRLF;

  AmBasicSipDialog::reply_error(req, 200, "OK", contact_hdr);
  return true;
}
