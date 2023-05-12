/*
 * Copyright (C) 2011 Stefan Sayer
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the sems software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "DBRegAgent.h"
#include "AmSession.h"
#include "AmEventDispatcher.h"

#include <unistd.h>
#include <stdlib.h>

EXPORT_MODULE_FACTORY(DBRegAgent);
DEFINE_MODULE_INSTANCE(DBRegAgent, MOD_NAME);

mysqlpp::Connection DBRegAgent::MainDBConnection(mysqlpp::use_exceptions);
mysqlpp::Connection DBRegAgent::ProcessorDBConnection(mysqlpp::use_exceptions);

string DBRegAgent::joined_query_subscribers;
string DBRegAgent::joined_query_peerings;
string DBRegAgent::registrations_table = "registrations";

double DBRegAgent::reregister_interval = 0.5;
double DBRegAgent::minimum_reregister_interval = -1;

bool DBRegAgent::enable_ratelimiting = false;
unsigned int DBRegAgent::ratelimit_rate = 0;
unsigned int DBRegAgent::ratelimit_per = 0;
bool DBRegAgent::ratelimit_slowstart = false;

bool DBRegAgent::delete_removed_registrations = true;
bool DBRegAgent::delete_failed_deregistrations = false;
bool DBRegAgent::save_contacts = true;
bool DBRegAgent::db_read_contact = false;
string DBRegAgent::contact_hostport;
bool DBRegAgent::username_with_domain = false;
string DBRegAgent::outbound_proxy;

bool DBRegAgent::save_auth_replies = false;

unsigned int DBRegAgent::error_retry_interval = 300;

static void _timer_cb(RegTimer* timer, long object_id, int data2, const string& type) {
  DBRegAgent::instance()->timer_cb(timer, object_id, data2, type);
}

DBRegAgent::DBRegAgent(const string& _app_name)
  : AmDynInvokeFactory(_app_name),
    AmEventQueue(this),
    uac_auth_i(NULL)
{
}

DBRegAgent::~DBRegAgent() {
}

int DBRegAgent::onLoad()
{

  DBG("loading db_reg_agent....\n");

  AmDynInvokeFactory* uac_auth_f = AmPlugIn::instance()->getFactory4Di("uac_auth");
  if (uac_auth_f == NULL) {
    WARN("unable to get a uac_auth factory. "
	 "registrations will not be authenticated.\n");
    WARN("(do you want to load uac_auth module?)\n");
  } else {
    uac_auth_i = uac_auth_f->getInstance();
  }

  AmConfigReader cfg;
  if(cfg.loadFile(add2path(AmConfig::ModConfigPath,1, MOD_NAME ".conf")))
    return -1;

  expires = cfg.getParameterInt("expires", 7200);
  DBG("requesting registration expires of %u seconds\n", expires);

  if (cfg.hasParameter("reregister_interval")) {
    reregister_interval = -1;
    reregister_interval = atof(cfg.getParameter("reregister_interval").c_str());
    if (reregister_interval <= 0 || reregister_interval > 1) {
      ERROR("configuration value 'reregister_interval' could not be read. "
	    "needs to be 0 .. 1.0 (recommended: 0.5)\n");
      return -1;
    }
  }

  if (cfg.hasParameter("minimum_reregister_interval")) {
    minimum_reregister_interval = -1;
    minimum_reregister_interval = atof(cfg.getParameter("minimum_reregister_interval").c_str());
    if (minimum_reregister_interval <= 0 || minimum_reregister_interval > 1) {
      ERROR("configuration value 'minimum_reregister_interval' could not be read. "
	    "needs to be 0 .. reregister_interval (recommended: 0.4)\n");
      return -1;
    }

    if (minimum_reregister_interval >= reregister_interval) {
      ERROR("configuration value 'minimum_reregister_interval' must be smaller "
	    "than reregister_interval (recommended: 0.4)\n");
      return -1;
    }
  }

  enable_ratelimiting = cfg.getParameter("enable_ratelimiting") == "yes";
  if (enable_ratelimiting) {
    if (!cfg.hasParameter("ratelimit_rate") || !cfg.hasParameter("ratelimit_per")) {
      ERROR("if ratelimiting is enabled, ratelimit_rate and ratelimit_per must be set\n");
      return -1;
    }
    ratelimit_rate = cfg.getParameterInt("ratelimit_rate", 0);
    ratelimit_per = cfg.getParameterInt("ratelimit_per", 0);
    if (!ratelimit_rate || !ratelimit_per) {
      ERROR("ratelimit_rate and ratelimit_per must be > 0\n");
      return -1;
    }
    ratelimit_slowstart = cfg.getParameter("ratelimit_slowstart") == "yes";

  }

  delete_removed_registrations =
    cfg.getParameter("delete_removed_registrations", "yes") == "yes";

  delete_failed_deregistrations =
    cfg.getParameter("delete_failed_deregistrations", "no") == "yes";

  save_contacts =
    cfg.getParameter("save_contacts", "yes") == "yes";

  db_read_contact =
    cfg.getParameter("db_read_contact", "no") == "yes";

  username_with_domain =
    cfg.getParameter("username_with_domain", "no") == "yes";

  save_auth_replies =
    cfg.getParameter("save_auth_replies", "no") == "yes";

  contact_hostport = cfg.getParameter("contact_hostport");

  outbound_proxy = cfg.getParameter("outbound_proxy");

  error_retry_interval = cfg.getParameterInt("error_retry_interval", 300);
  if (!error_retry_interval) {
    WARN("disabled retry on errors!\n");
  }

  string mysql_server, mysql_user, mysql_passwd, mysql_db;

  mysql_server = cfg.getParameter("mysql_server", "localhost");

  mysql_user = cfg.getParameter("mysql_user");
  if (mysql_user.empty()) {
    ERROR(MOD_NAME ".conf parameter 'mysql_user' is missing.\n");
    return -1;
  }

  mysql_passwd = cfg.getParameter("mysql_passwd");
  if (mysql_passwd.empty()) {
    ERROR(MOD_NAME ".conf parameter 'mysql_passwd' is missing.\n");
    return -1;
  }

  mysql_db = cfg.getParameter("mysql_db", "sems");

  try {

    MainDBConnection.set_option(new mysqlpp::ReconnectOption(true));
    // matched instead of changed rows in result, so we know when to create DB entry
    MainDBConnection.set_option(new mysqlpp::FoundRowsOption(true));
    MainDBConnection.connect(mysql_db.c_str(), mysql_server.c_str(),
                      mysql_user.c_str(), mysql_passwd.c_str());
    if (!MainDBConnection) {
      ERROR("Database connection failed: %s\n", MainDBConnection.error());
      return -1;
    }

    ProcessorDBConnection.set_option(new mysqlpp::ReconnectOption(true));
    // matched instead of changed rows in result, so we know when to create DB entry
    ProcessorDBConnection.set_option(new mysqlpp::FoundRowsOption(true));
    ProcessorDBConnection.connect(mysql_db.c_str(), mysql_server.c_str(),
                      mysql_user.c_str(), mysql_passwd.c_str());
    if (!ProcessorDBConnection) {
      ERROR("Database connection failed: %s\n", ProcessorDBConnection.error());
      return -1;
    }

  } catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return -1;
  }

  // register us as SIP event receiver for MOD_NAME
  AmEventDispatcher::instance()->addEventQueue(MOD_NAME,this);

  if (!AmPlugIn::registerDIInterface(MOD_NAME, this)) {
    ERROR("registering %s DI interface\n", MOD_NAME);
    return -1;
  }

  joined_query_subscribers = cfg.getParameter("joined_query_subscribers");
  joined_query_peerings = cfg.getParameter("joined_query_peerings");
  if (joined_query_subscribers.empty() || joined_query_peerings.empty()) {
    // todo: name!
    ERROR("joined_query must be set\n");
    return -1;
  }

  if (cfg.hasParameter("registrations_table")) {
    registrations_table = cfg.getParameter("registrations_table");
  }
  DBG("using registrations table '%s'\n", registrations_table.c_str());

  if (!loadRegistrations()) {
    ERROR("REGISTER: loading registrations for subscribers from DB\n");
    return -1;
  }
  if (!loadRegistrationsPeerings()) {
    ERROR("REGISTER: loading registrations for peerings from DB\n");
    return -1;
  }

  DBG("starting registration timer thread...\n");
  registration_scheduler.start();

  // run_tests();

  start();

  return 0;
}

void DBRegAgent::onUnload() {
  if (running) {
    running = false;
    registration_scheduler._timer_thread_running = false;
    DBG("unclean shutdown. Waiting for processing thread to stop.\n");
    for (int i=0;i<400;i++) {
      if (shutdown_finished && registration_scheduler._shutdown_finished)
	break;
      usleep(2000); // 2ms
    }

    if (!shutdown_finished || !registration_scheduler._shutdown_finished) {
      WARN("processing thread could not be stopped, process will probably crash\n");
    }
  }

  DBG("closing main DB connection\n");
  MainDBConnection.disconnect();
  DBG("closing auxiliary DB connection\n");
  ProcessorDBConnection.disconnect();
}

bool DBRegAgent::loadRegistrations() {
  try {
    time_t now_time = time(NULL);

    mysqlpp::Query query_sb = DBRegAgent::MainDBConnection.query();
    string query_string_subscribers, table;
    query_string_subscribers = joined_query_subscribers;

    DBG("REGISTER: querying all registrations for subscribers with : '%s'\n", query_string_subscribers.c_str());

    query_sb << query_string_subscribers;
    mysqlpp::UseQueryResult res_sb = query_sb.use();

    while (mysqlpp::Row row = res_sb.fetch_row()) {
      int status = 0;
      string type = TYPE_SUBSCRIBER;
      long object_id = row[COLNAME_SUBSCRIBER_ID];

      if (object_id == 0) {
        WARN("REGISTER: object_id is 0 for this subscriber, skipping..\n");
        continue;
      }

			DBG("REGISTER: Triggering for subscriber with object_id=<%ld>\n", object_id);

      string contact_uri;
      if (db_read_contact && row[COLNAME_CONTACT] != mysqlpp::null) {
        contact_uri = (string) row[COLNAME_CONTACT];
      }

      if (row[COLNAME_STATUS] != mysqlpp::null)
        status = row[COLNAME_STATUS];
      else {
        DBG("registration status entry for id %ld does not exist, creating...\n",
        object_id);
        createDBRegistration(object_id, type, ProcessorDBConnection);
      }

      DBG("got subscriber '%s@%s' status %i\n",
      string(row[COLNAME_USER]).c_str(), string(row[COLNAME_REALM]).c_str(), status);

      switch (status) {
        case REG_STATUS_INACTIVE:
        case REG_STATUS_PENDING: // try again
        case REG_STATUS_FAILED:  // try again
        {
          createRegistration(object_id,
                (string)row[COLNAME_AUTH_USER],
                (string)row[COLNAME_USER],
                (string)row[COLNAME_PASS],
                (string)row[COLNAME_REALM],
                contact_uri,
                type);
          scheduleRegistration(object_id, type);
        }; break;

        case REG_STATUS_ACTIVE:
        {
          createRegistration(object_id,
                (string)row[COLNAME_AUTH_USER],
                (string)row[COLNAME_USER],
                (string)row[COLNAME_PASS],
                (string)row[COLNAME_REALM],
                contact_uri,
                type);

          time_t dt_expiry = now_time;
          if (row[COLNAME_EXPIRY] != mysqlpp::null) {
            dt_expiry = (time_t)((mysqlpp::DateTime)row[COLNAME_EXPIRY]);
          }

          time_t dt_registration_ts = now_time;
          if (row[COLNAME_REGISTRATION_TS] != mysqlpp::null) {
            dt_registration_ts = (time_t)((mysqlpp::DateTime)row[COLNAME_REGISTRATION_TS]);
          }

          DBG("got expiry '%ld, registration_ts %ld, now %ld'\n",
              dt_expiry, dt_registration_ts, now_time);

          if (dt_registration_ts > now_time) {
            WARN("needed to sanitize last_registration timestamp TS from the %ld (now %ld) - "
          "DB host time mismatch?\n", dt_registration_ts, now_time);
            dt_registration_ts = now_time;
          }

          // if expired add to pending registrations, else schedule re-regstration
          if (dt_expiry <= now_time) {
            DBG("scheduling imminent re-registration for subscriber %ld\n", object_id);
            scheduleRegistration(object_id, type);
          } else {
            setRegistrationTimer(object_id, dt_expiry, dt_registration_ts, now_time, type);
          }
        }; break;

        case REG_STATUS_REMOVED:
        {
          DBG("ignoring removed registration %ld %s@%s of type: %s\n", object_id,
              ((string)row[COLNAME_USER]).c_str(), ((string)row[COLNAME_REALM]).c_str(), type.c_str());
        } break;

        case REG_STATUS_TO_BE_REMOVED:
        {
          DBG("Scheduling Deregister of registration %ld %s@%s of type: %s\n", object_id,
              ((string)row[COLNAME_USER]).c_str(), ((string)row[COLNAME_REALM]).c_str(), type.c_str());
          createRegistration(object_id,
                (string)row[COLNAME_AUTH_USER],
                (string)row[COLNAME_USER],
                (string)row[COLNAME_PASS],
                (string)row[COLNAME_REALM],
                contact_uri,
                type);
          scheduleDeregistration(object_id, type);
        };
      }
    }

  } catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return false;
  }

  return true;
}

bool DBRegAgent::loadRegistrationsPeerings() {
  try {
    time_t now_time = time(NULL);

    mysqlpp::Query query_pr = DBRegAgent::MainDBConnection.query();
    string query_string_peerings, table;
    query_string_peerings = joined_query_peerings;

    DBG("REGISTER: querying all registrations for peerings with : '%s'\n", query_string_peerings.c_str());

    query_pr << query_string_peerings;
    mysqlpp::UseQueryResult res_pr = query_pr.use();

    while (mysqlpp::Row row = res_pr.fetch_row()) {
      int status = 0;
      string type = TYPE_PEERING;
      long object_id = row[COLNAME_PEER_ID];

      if (object_id == 0) {
        WARN("REGISTER: object_id is NULL or 0 for this peering, skipping..\n");
        continue;
      }

      DBG("REGISTER: Triggering for peering with object_id=<%ld>\n", object_id);

      string contact_uri;
      if (db_read_contact && row[COLNAME_CONTACT] != mysqlpp::null) {
        contact_uri = (string) row[COLNAME_CONTACT];
      }

      if (row[COLNAME_STATUS] != mysqlpp::null)
        status = row[COLNAME_STATUS];
      else {
        DBG("registration status entry for id %ld does not exist, creating...\n",
        object_id);
        createDBRegistration(object_id, type, ProcessorDBConnection);
      }

      DBG("got subscriber '%s@%s' status %i\n",
      string(row[COLNAME_USER]).c_str(), string(row[COLNAME_REALM]).c_str(), status);

      switch (status) {
        case REG_STATUS_INACTIVE:
        case REG_STATUS_PENDING: // try again
        case REG_STATUS_FAILED:  // try again
        {
          createRegistration(object_id,
                (string)row[COLNAME_AUTH_USER],
                (string)row[COLNAME_USER],
                (string)row[COLNAME_PASS],
                (string)row[COLNAME_REALM],
                contact_uri,
                type);
          scheduleRegistration(object_id, type);
        }; break;

        case REG_STATUS_ACTIVE:
        {
          createRegistration(object_id,
                (string)row[COLNAME_AUTH_USER],
                (string)row[COLNAME_USER],
                (string)row[COLNAME_PASS],
                (string)row[COLNAME_REALM],
                contact_uri,
                type);

          time_t dt_expiry = now_time;
          if (row[COLNAME_EXPIRY] != mysqlpp::null) {
            dt_expiry = (time_t)((mysqlpp::DateTime)row[COLNAME_EXPIRY]);
          }

          time_t dt_registration_ts = now_time;
          if (row[COLNAME_REGISTRATION_TS] != mysqlpp::null) {
            dt_registration_ts = (time_t)((mysqlpp::DateTime)row[COLNAME_REGISTRATION_TS]);
          }

          DBG("got expiry '%ld, registration_ts %ld, now %ld'\n",
              dt_expiry, dt_registration_ts, now_time);

          if (dt_registration_ts > now_time) {
            WARN("needed to sanitize last_registration timestamp TS from the %ld (now %ld) - "
          "DB host time mismatch?\n", dt_registration_ts, now_time);
            dt_registration_ts = now_time;
          }

          // if expired add to pending registrations, else schedule re-regstration
          if (dt_expiry <= now_time) {
            DBG("scheduling imminent re-registration for subscriber %ld\n", object_id);
            scheduleRegistration(object_id, type);
          } else {
            setRegistrationTimer(object_id, dt_expiry, dt_registration_ts, now_time, type);
          }
        }; break;

        case REG_STATUS_REMOVED:
        {
          DBG("ignoring removed registration %ld %s@%s of type: %s\n", object_id,
              ((string)row[COLNAME_USER]).c_str(), ((string)row[COLNAME_REALM]).c_str(), type.c_str());
        } break;

        case REG_STATUS_TO_BE_REMOVED:
        {
          DBG("Scheduling Deregister of registration %ld %s@%s of type: %s\n", object_id,
              ((string)row[COLNAME_USER]).c_str(), ((string)row[COLNAME_REALM]).c_str(), type.c_str());
          createRegistration(object_id,
                (string)row[COLNAME_AUTH_USER],
                (string)row[COLNAME_USER],
                (string)row[COLNAME_PASS],
                (string)row[COLNAME_REALM],
                contact_uri,
                type);
          scheduleDeregistration(object_id, type);
        };
      }
    }

  } catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return false;
  }

  return true;
}

/** create registration in our list */
void DBRegAgent::createRegistration(long object_id,
            const string& auth_user,
            const string& user,
            const string& pass,
            const string& realm,
            const string& contact,
            const string& type) {

  string auth_user_temp = (auth_user.empty() || auth_user == "" || auth_user == "NULL") ? user : auth_user;
	DBG("REGISTER: authentication user picked out: <%s> \n", auth_user_temp.c_str());

  string _user = user;
  if (username_with_domain && user.find('@')!=string::npos) {
    _user = user.substr(0, user.find('@'));
  }

  string contact_uri = contact;
  if (contact_uri.empty() && !contact_hostport.empty()) {
    contact_uri = "sip:"+ _user + "@" + contact_hostport;
  }

  string handle = AmSession::getNewId();
  SIPRegistrationInfo reg_info(realm, _user,
			       _user, // name
			       auth_user_temp, // auth_user
			       pass,
			       outbound_proxy, // proxy
			       contact_uri // contact
			       );

  DBG(" >>> realm '%s', user '%s', auth_user '%s', pass '%s', outbound_proxy '%s', contact_uri '%s', type '%s'\n",
      realm.c_str(), user.c_str(), auth_user.c_str(), pass.c_str(),
      outbound_proxy.c_str(), contact_uri.c_str(), type.c_str());

  registrations_mut.lock();
  try {
    // remove already existing registration for a peering
    if (type == TYPE_PEERING) {
      if (registrations_peers.find(object_id) != registrations_peers.end()) {
        registrations_mut.unlock();
        WARN("registration for a Peering with ID %ld already exists, removing\n", object_id);
        removeRegistration(object_id, type);
        clearRegistrationTimer(object_id, type);
        registrations_mut.lock();
      }
    // remove already existing for a usual subscriber
    } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
      if (registrations.find(object_id) != registrations.end()) {
        registrations_mut.unlock();
        WARN("registration for a Subscriber with ID %ld already exists, removing\n", object_id);
        removeRegistration(object_id, type);
        clearRegistrationTimer(object_id, type);
        registrations_mut.lock();
      }
    }

    AmSIPRegistration* reg = new AmSIPRegistration(handle, reg_info, "" /*MOD_NAME*/);

    // a simple fix in case expires is for some reason 0. Not the best solution.
    //if (expires == 0) expires = 60;
    reg->setExpiresInterval(expires);

    if (type == TYPE_PEERING) {
      registrations_peers[object_id] = reg;
      registration_ltags_peers[handle] = object_id;
    } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
      registrations[object_id] = reg;
      registration_ltags[handle] = object_id;
    }

    if (NULL != uac_auth_i) {
      DBG("REGISTER: Enabling UAC Auth for new registration of type: <%s>\n", type.c_str());
      
      // get a sessionEventHandler from uac_auth
      AmArg di_args, ret;
      AmArg a;
      a.setBorrowedPointer(reg);
      di_args.push(a);
      di_args.push(a);
      
      uac_auth_i->invoke("getHandler", di_args, ret);
      if (!ret.size()) {
        ERROR("Can not add auth handler to new registration!\n");
      } else {
        AmObject* p = ret.get(0).asObject();
        if (p != NULL) {
          AmSessionEventHandler* h = dynamic_cast<AmSessionEventHandler*>(p);
	        if (h != NULL) reg->setSessionEventHandler(h);
        }
      }
    }
  } catch (const std::exception& e) {
    ERROR("%s", e.what());
  } catch (...) {
    ERROR("unknown exception occured\n");
  }

  registrations_mut.unlock();

  // register us as SIP event receiver for this ltag
  AmEventDispatcher::instance()->addEventQueue(handle,this);

  DBG("created new registration with ID <%ld>, ltag '%s' and type '%s'\n",
      object_id, handle.c_str(), type.c_str());
}

void DBRegAgent::updateRegistration(long object_id,
				    const string& auth_user,
				    const string& user,
				    const string& pass,
				    const string& realm,
            const string& contact,
            const string& type) {

  string auth_user_temp = (auth_user.empty() || auth_user == "" || auth_user == "NULL") ? user : auth_user;
  DBG("REGISTER: authentication user picked out: <%s> \n", auth_user_temp.c_str());

  string _user = user;
  if (username_with_domain && user.find('@')!=string::npos) {
    _user = user.substr(0, user.find('@'));
  }

  registrations_mut.lock();

  map<long, AmSIPRegistration*>::iterator it;

  if (type == TYPE_PEERING) {
    it=registrations_peers.find(object_id);
    if (it == registrations_peers.end()) {
      registrations_mut.unlock();
      WARN("updateRegistration - registration %ld %s@%s unknown, creating. Type: %s\n",
          object_id, user.c_str(), realm.c_str(), type.c_str());
      createRegistration(object_id, auth_user_temp, user, pass, realm, contact, type);
      scheduleRegistration(object_id, type);
      return;
    }
  } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
    it=registrations.find(object_id);
    if (it == registrations.end()) {
      registrations_mut.unlock();
      WARN("updateRegistration - registration %ld %s@%s unknown, creating. Type: %s\n",
          object_id, user.c_str(), realm.c_str(), type.c_str());
      createRegistration(object_id, auth_user_temp, user, pass, realm, contact, type);
      scheduleRegistration(object_id, type);
      return;
    }
  }

  bool need_reregister = it->second->getInfo().domain != realm
    || it->second->getInfo().auth_user != auth_user_temp
    || it->second->getInfo().user != _user
    || it->second->getInfo().pwd  != pass
    || it->second->getInfo().contact != contact;

  string old_realm = it->second->getInfo().domain;
  string old_user = it->second->getInfo().user;
  string old_auth_user = it->second->getInfo().auth_user;
  it->second->setRegistrationInfo(SIPRegistrationInfo(realm, _user,
						      _user, // name
						      auth_user_temp, // auth_user
						      pass,
						      outbound_proxy,   // proxy
						      contact)); // contact
  registrations_mut.unlock();
  if (need_reregister) {
    DBG("user/realm for registration %ld changed (%s@%s -> %s@%s). Auth user (%s -> %s)."
        "Triggering immediate re-registration\n",
        object_id, old_user.c_str(), old_realm.c_str(),
        user.c_str(), realm.c_str(), old_auth_user.c_str(), auth_user_temp.c_str());
    scheduleRegistration(object_id, type);
  }
}

/** remove registration from our list */
void DBRegAgent::removeRegistration(long object_id, const string& type) {
  bool res = false;
  string handle;
  registrations_mut.lock();

  map<long, AmSIPRegistration*>::iterator it;
  if (type == TYPE_PEERING) {                                       // remove reg for peerings
    it = registrations_peers.find(object_id);
    if (it != registrations_peers.end()) {
      handle = it->second->getHandle();
      registration_ltags_peers.erase(handle);
      delete it->second;
      registrations_peers.erase(it);
      res = true;
    }
  } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {   // remove reg for subscribers
    it = registrations.find(object_id);
    if (it != registrations.end()) {
      handle = it->second->getHandle();
      registration_ltags.erase(handle);
      delete it->second;
      registrations.erase(it);
      res = true;
    }
  }

  registrations_mut.unlock();

  if (res) {
    // deregister us as SIP event receiver for this ltag
    AmEventDispatcher::instance()->delEventQueue(handle);
    DBG("removed registration with ID %ld, type: %s \n", object_id, type.c_str());
  } else {
    DBG("registration with ID %ld not found for removing, type: %s \n", object_id, type.c_str());
  }
}

/** schedule this registration to REGISTER (immediately) */
void DBRegAgent::scheduleRegistration(long object_id, const string& type) {
  if (enable_ratelimiting) {
    registration_processor.
    postEvent(new RegistrationActionEvent(RegistrationActionEvent::Register,
                object_id, type));
  } else {
    // use our own thread
    postEvent(new RegistrationActionEvent(RegistrationActionEvent::Register,
                object_id, type));
  }
  DBG("Added to pending actions: REGISTER of %ld, type: %s\n", object_id, type.c_str());
}

/** schedule this registration to de-REGISTER (immediately) */
void DBRegAgent::scheduleDeregistration(long object_id, const string& type) {
  if (enable_ratelimiting) {
    registration_processor.
      postEvent(new RegistrationActionEvent(RegistrationActionEvent::Deregister,
                object_id, type));
  } else {
    // use our own thread
      postEvent(new RegistrationActionEvent(RegistrationActionEvent::Deregister,
                object_id, type));
  }
  DBG("added to pending actions: DEREGISTER of %ld, type: %s\n", object_id, type.c_str());
}

void DBRegAgent::process(AmEvent* ev) {

  if (ev->event_id == RegistrationActionEventID) {
    RegistrationActionEvent* reg_action_ev =
      dynamic_cast<RegistrationActionEvent*>(ev);
    if (reg_action_ev) {
      onRegistrationActionEvent(reg_action_ev);
      return;
    }
  }

  AmSipReplyEvent* sip_rep = dynamic_cast<AmSipReplyEvent*>(ev);
  if (sip_rep) {
      onSipReplyEvent(sip_rep);
    return;
  }

  if (ev->event_id == E_SYSTEM) {
    AmSystemEvent* sys_ev = dynamic_cast<AmSystemEvent*>(ev);
    if(sys_ev){	
      DBG("Session received system Event\n");
      if (sys_ev->sys_event == AmSystemEvent::ServerShutdown) {
	running = false;
	registration_scheduler._timer_thread_running = false;
      }
      return;
    }
  }

  ERROR("unknown event received!\n");
}

// uses ProcessorDBConnection
void DBRegAgent::onRegistrationActionEvent(RegistrationActionEvent* reg_action_ev) {
  switch (reg_action_ev->action) {

  case RegistrationActionEvent::Register:
    {
      DBG("REGISTER of registration %ld, type: %s\n",
          reg_action_ev->object_id, reg_action_ev->type.c_str());

      registrations_mut.lock();
      map<long, AmSIPRegistration*>::iterator it;
      bool marker = true;

      if (reg_action_ev->type == TYPE_PEERING) {
        it = registrations_peers.find(reg_action_ev->object_id);
        if (it==registrations_peers.end()) {
          DBG("ignoring scheduled REGISTER of unknown registration %ld\n",
          reg_action_ev->object_id);
          marker = false;
        }
      } else if (reg_action_ev->type == TYPE_SUBSCRIBER || reg_action_ev->type == TYPE_UNDEFINED) {
        it = registrations.find(reg_action_ev->object_id);
        if (it==registrations.end()) {
          DBG("ignoring scheduled REGISTER of unknown registration %ld\n",
          reg_action_ev->object_id);
          marker = false;
        }
      }

      if (marker) {
        if (!it->second->doRegistration()) {
            updateDBRegistration(ProcessorDBConnection,
            reg_action_ev->object_id, reg_action_ev->type,
            480, ERR_REASON_UNABLE_TO_SEND_REQUEST,
            true, REG_STATUS_FAILED);
          if (error_retry_interval) {
            // schedule register-refresh after error_retry_interval
            setRegistrationTimer(reg_action_ev->object_id, error_retry_interval,
              RegistrationActionEvent::Register, reg_action_ev->type);
          }
        }
      }

      registrations_mut.unlock();
    } break;

  case RegistrationActionEvent::Deregister:
    {
      DBG("De-REGISTER of registration %ld, type: %s\n",
          reg_action_ev->object_id, reg_action_ev->type.c_str());

      registrations_mut.lock();
      map<long, AmSIPRegistration*>::iterator it;
      bool marker = true;

      if (reg_action_ev->type == TYPE_PEERING) {
        it = registrations_peers.find(reg_action_ev->object_id);
        if (it==registrations_peers.end()) {
          DBG("ignoring scheduled De-REGISTER of unknown registration %ld\n",
          reg_action_ev->object_id);
          marker = false;
        }
      } else if (reg_action_ev->type == TYPE_SUBSCRIBER || reg_action_ev->type == TYPE_UNDEFINED) {
        it = registrations.find(reg_action_ev->object_id);
        if (it==registrations.end()) {
          DBG("ignoring scheduled De-REGISTER of unknown registration %ld\n",
          reg_action_ev->object_id);
          marker = false;
        }
      }

      if (marker) {
        if (!it->second->doUnregister()) {
          if (delete_removed_registrations && delete_failed_deregistrations) {
            DBG("sending de-Register failed - deleting registration %ld "
                "(delete_failed_deregistrations=yes)\n", reg_action_ev->object_id);
            deleteDBRegistration(reg_action_ev->object_id, reg_action_ev->type, ProcessorDBConnection);
          } else {
            DBG("failed sending de-register, updating DB with REG_STATUS_TO_BE_REMOVED "
                ERR_REASON_UNABLE_TO_SEND_REQUEST "for subscriber %ld\n",
                reg_action_ev->object_id);
            updateDBRegistration(ProcessorDBConnection,
              reg_action_ev->object_id, reg_action_ev->type,
              480, ERR_REASON_UNABLE_TO_SEND_REQUEST,
              true, REG_STATUS_TO_BE_REMOVED);
          }
        }
      }

      registrations_mut.unlock();
    } break;
  }
}

void DBRegAgent::createDBRegistration(long object_id, const string& type, mysqlpp::Connection& conn) {

  string column_id = COLNAME_SUBSCRIBER_ID;
  if (type == TYPE_PEERING) column_id = COLNAME_PEER_ID;

  // depending on if that is a registration for a subscriber or for a peering
  // do a mysql insertion
  string insert_query = "insert into "+registrations_table+
    " (" + column_id.c_str() + ")" + "values ("+ long2str(object_id)+");";

  DBG("MYSQL: trying to execute: <%s>\n", insert_query.c_str());

  try {
    mysqlpp::Query query = conn.query();
    query << insert_query;

    mysqlpp::SimpleResult res = query.execute();
    if (!res) {
      WARN("creating registration in DB with query '%s' failed: '%s', type: %s\n",
          insert_query.c_str(), res.info(), type.c_str());
    }
  }  catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return;
  }
}

void DBRegAgent::deleteDBRegistration(long object_id, const string& type, mysqlpp::Connection& conn) {

  string column_id = COLNAME_SUBSCRIBER_ID;
  if (type == TYPE_PEERING) column_id = COLNAME_PEER_ID;

  // depending on if that is a de-registration for a subscriber or for a peering
  // do a mysql deletion
  string insert_query = "delete from "+registrations_table+
    " where " + column_id.c_str() + "=" + long2str(object_id) + ";";

  try {
    mysqlpp::Query query = conn.query();
    query << insert_query;

    mysqlpp::SimpleResult res = query.execute();
    if (!res) {
      WARN("removing registration in DB with query '%s' failed: '%s', type: %s\n",
          insert_query.c_str(), res.info(), type.c_str());
    }
  }  catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return;
  }
}

void DBRegAgent::updateDBRegistration(mysqlpp::Connection& db_connection,
				      long object_id, const string& type, int last_code,
				      const string& last_reason,
				      bool update_status, int status,
				      bool update_ts, unsigned int expiry,
				      bool update_contacts, const string& contacts) {
  try {

    mysqlpp::Query query = db_connection.query();

    query << "update "+registrations_table+" set last_code="+ int2str(last_code) +", ";
    query << "last_reason=";
    query << mysqlpp::quote << last_reason;

    if (update_status) {
      query <<  ", registration_status="+int2str(status);
    }

    if (update_ts) {
      query << ", last_registration=NOW(), "
	"expiry=TIMESTAMPADD(SECOND,"+int2str(expiry)+", NOW())";
    }

    if (update_contacts) {
      query << ", contacts=" << mysqlpp::quote << contacts;
    }

    // depending on if that is an update for a subscriber or for a peering
    // do a mysql update
    if (type == TYPE_SUBSCRIBER)
      query << " where " COLNAME_SUBSCRIBER_ID "="+long2str(object_id) + ";";
    else
      query << " where " COLNAME_PEER_ID "="+long2str(object_id) + ";";
    string query_str = query.str();
    DBG("updating registration in DB with query '%s'\n", query_str.c_str());

    mysqlpp::SimpleResult res = query.execute();
    if (!res) {
      WARN("updating registration in DB with query '%s' failed: '%s'\n",
	   query_str.c_str(), res.info());
    } else {
      if (!res.rows()) {
	// should not happen - DB entry is created on load or on createRegistration
  DBG("creating registration DB entry for subscriber %ld, type: %s\n", object_id, type.c_str());
  createDBRegistration(object_id, type, db_connection);
	query.reset();
	query << query_str;

	mysqlpp::SimpleResult res = query.execute();
	if (!res || !res.rows()) {
	  WARN("updating registration in DB with query '%s' failed: '%s'\n",
	       query_str.c_str(), res.info());
	}
      }
    }

  }  catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return;
  }

}

// uses MainDBConnection
void DBRegAgent::onSipReplyEvent(AmSipReplyEvent* ev) {
  if (!ev) return;

  DBG("received SIP reply event for '%s'\n", 
#ifdef HAS_OFFER_ANSWER
      ev->reply.from_tag.c_str()
#else
      ev->reply.local_tag.c_str()
#endif
      );
  registrations_mut.lock();

  string local_tag =
#ifdef HAS_OFFER_ANSWER
    ev->reply.from_tag;
#else
    ev->reply.local_tag;
#endif

  map<string, long>::iterator it;

  bool marker = false;
  string type;

  // not the best solution to match coming reply against needed object
  // we need to find a way how to better differentiate,
  // if that is related to a peering type or subscriber type
  // now a basic attempt to first look into subscribers cache buckets then into
  // peerings cache buckets (can this happen that ltag will overlap? must be unique)


  // first try to find a registration object in a cache for subscribers (most common case)
  it=registration_ltags.find(local_tag);
  if (it!=registration_ltags.end()) {
    marker=true;
    type = TYPE_SUBSCRIBER;

  // secondly, if we didn't find anything before, try in a cache for peerings
  } else {
    it=registration_ltags_peers.find(local_tag);
    if (it!=registration_ltags_peers.end()) {
      marker=true;
      type = TYPE_PEERING;
    }
  }

  if (marker) {
    long object_id = it->second;
    map<long, AmSIPRegistration*>::iterator r_it;

    marker = false;
    if (type == TYPE_SUBSCRIBER) {                           // find registration for peering
      r_it=registrations.find(object_id);
      if (r_it != registrations.end()) marker = true;
    } else {                                                 // find registration for subscriber
      r_it=registrations_peers.find(object_id);
      if (r_it != registrations_peers.end()) marker = true;
    }

    if (marker) {
      AmSIPRegistration* registration = r_it->second;
      if (!registration) {
        ERROR("Internal error: registration object missing, type: %s\n", type.c_str());
        return;
      }
      unsigned int cseq_before = registration->getDlg()->cseq;

#ifdef HAS_OFFER_ANSWER
      registration->getDlg()->onRxReply(ev->reply);
#else
      registration->getDlg()->updateStatus(ev->reply);
#endif

      //update registrations set
      bool update_status = false;
      int status = 0;
      bool update_ts = false;
      unsigned int expiry = 0;
      bool delete_status = false;
      bool auth_pending = false;

      if (ev->reply.code >= 300) {
        // REGISTER or de-REGISTER failed
        if ((ev->reply.code == 401 || ev->reply.code == 407) &&
            // auth response codes
            // processing reply triggered sending request: resent by auth
            (cseq_before != registration->getDlg()->cseq)) {
          DBG("received negative reply, but still in pending state (auth).\n");
          auth_pending = true;
        } else {
          if (!registration->getUnregistering()) {
            // REGISTER failed - mark in DB
            DBG("registration failed - mark in DB\n");
            update_status = true;
            status = REG_STATUS_FAILED;
            if (error_retry_interval) {
              // schedule register-refresh after error_retry_interval
              setRegistrationTimer(object_id, error_retry_interval,
                RegistrationActionEvent::Register, type);
            }
          } else {
            // de-REGISTER failed
            if (delete_removed_registrations && delete_failed_deregistrations) {
              DBG("de-Register failed - deleting registration %ld "
            "(delete_failed_deregistrations=yes)\n", object_id);
              delete_status = true;
            } else {
              update_status = true;
              status = REG_STATUS_TO_BE_REMOVED;
            }
          }
        }
      } else if (ev->reply.code >= 200) {
        // positive reply
        if (!registration->getUnregistering()) {
          time_t now_time = time(0);
          setRegistrationTimer(object_id, registration->getExpiresTS(),
                  now_time, now_time, type);

          update_status = true;
          status = REG_STATUS_ACTIVE;

          update_ts = true;
          expiry = registration->getExpiresLeft();
        } else {
          if (delete_removed_registrations) {
            delete_status = true;
          } else {
            update_status = true;
            status = REG_STATUS_REMOVED;
          }
        }
      }

      // skip provisional replies & auth
      if (ev->reply.code >= 200 && !auth_pending) {
        // remove unregistered
        if (registration->getUnregistering()) {
          registrations_mut.unlock();
          removeRegistration(object_id, type);
          registrations_mut.lock();
        }
      }

      if (!delete_status) {
        if (auth_pending && !save_auth_replies) {
          DBG("not updating DB with auth reply %u %s\n",
              ev->reply.code, ev->reply.reason.c_str());
        } else {
          DBG("update DB with reply %u %s\n", ev->reply.code, ev->reply.reason.c_str());
          updateDBRegistration(MainDBConnection,
                  object_id, type, ev->reply.code, ev->reply.reason,
                  update_status, status, update_ts, expiry,
                  save_contacts, ev->reply.contact);
        }
      } else {
        DBG("delete DB registration of subscriber %ld\n", object_id);
        deleteDBRegistration(object_id, type, MainDBConnection);
      }

    } else {
      ERROR("internal: inconsistent registration list\n");
    }
  } else {
    DBG("ignoring reply for unknown registration\n");
  }
  registrations_mut.unlock();
}

void DBRegAgent::run() {
  running = shutdown_finished = true;

  DBG("DBRegAgent thread: waiting 2 sec for server startup ...\n");
  sleep(2);
  
  mysqlpp::Connection::thread_start();

  if (enable_ratelimiting) {
    DBG("starting processor thread\n");
    registration_processor.start();
  }

  DBG("running DBRegAgent thread...\n");
  shutdown_finished = false;
  while (running) {
    waitForEventTimed(500); // 500 milliseconds
    processEvents();
  }

  DBG("DBRegAgent done, removing all registrations from Event Dispatcher for peerings...\n");
  registrations_mut.lock();
  for (map<string, long>::iterator it=registration_ltags_peers.begin();
       it != registration_ltags_peers.end(); it++) {
    AmEventDispatcher::instance()->delEventQueue(it->first);
  }
  registrations_mut.unlock();

  DBG("removing %s registrations from Event Dispatcher...\n", MOD_NAME);
  AmEventDispatcher::instance()->delEventQueue(MOD_NAME);

  mysqlpp::Connection::thread_end();

  DBG("DBRegAgent thread stopped.\n");
  shutdown_finished = true;
}

void DBRegAgent::on_stop() {
  DBG("DBRegAgent on_stop()...\n");
  running = false;
}

void DBRegAgent::setRegistrationTimer(long object_id, unsigned int timeout,
				      RegistrationActionEvent::RegAction reg_action, const string& type) {
  DBG("setting Register timer for subscription %ld, timeout %u, reg_action %u\n",
      object_id, timeout, reg_action);

  RegTimer* timer = NULL;
  map<long, RegTimer*>::iterator it;
  bool marker = false;

  if (type == TYPE_PEERING) {
    it=registration_timers_peers.find(object_id);
    if (it==registration_timers_peers.end()) marker = true;

  } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
    it=registration_timers.find(object_id);
    if (it==registration_timers.end()) marker = true;
  }

  if (marker) {
    DBG("timer object for subscription %ld not found, type: %s\n", object_id, type.c_str());
    timer = new RegTimer();
    timer->data1 = object_id;
    timer->data3 = type;          // 'peering' or 'subscriber'
    timer->cb = _timer_cb;
    DBG("created timer object [%p] for subscription %ld, type: %s\n", timer, object_id, type.c_str());
  } else {
    timer = it->second;
    DBG("removing scheduled timer...\n");
    registration_scheduler.remove_timer(timer);
  }

  timer->data2 = reg_action;
  timer->expires = time(0) + timeout;

  DBG("placing timer for %ld in T-%u, type: %s\n", object_id, timeout, type.c_str());
  registration_scheduler.insert_timer(timer);

  if (type == TYPE_PEERING)
    registration_timers_peers.insert(std::make_pair(object_id, timer));
  else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED)
    registration_timers.insert(std::make_pair(object_id, timer));
}

void DBRegAgent::setRegistrationTimer(long object_id,
              time_t expiry, time_t reg_start_ts,
              time_t now_time, const string& type) {

  DBG("setting re-Register timer for subscription %ld, expiry %ld, reg_start_t %ld, type: %s\n",
      object_id, expiry, reg_start_ts, type.c_str());

  RegTimer* timer = NULL;
  map<long, RegTimer*>::iterator it;
  bool marker = false;

  if (type == TYPE_PEERING) {
    it=registration_timers_peers.find(object_id);
    if (it==registration_timers_peers.end()) marker = true;
  } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
    it=registration_timers.find(object_id);
    if (it==registration_timers.end()) marker = true;
  }

  if (marker) {
    DBG("timer object for subscription %ld not found, type: %s\n", object_id, type.c_str());
    timer = new RegTimer();
    timer->data1 = object_id;
    timer->data3 = type;          // 'peering' or 'subscriber'
    timer->cb = _timer_cb;
    DBG("created timer object [%p] for subscription %ld, type: %s\n", timer, object_id, type.c_str());
    registration_timers.insert(std::make_pair(object_id, timer));
  } else {
    timer = it->second;
    DBG("removing scheduled timer...\n");
    registration_scheduler.remove_timer(timer);
  }

  timer->data2 = RegistrationActionEvent::Register;

  if (minimum_reregister_interval>0.0) {
    time_t t_expiry_max = reg_start_ts;
    time_t t_expiry_min = reg_start_ts;
    if (expiry > reg_start_ts)
      t_expiry_max+=(expiry - reg_start_ts) * reregister_interval;
    if (expiry > reg_start_ts)
      t_expiry_min+=(expiry - reg_start_ts) * minimum_reregister_interval;

    if (t_expiry_max < now_time) {
      // calculated interval completely in the past - immediate re-registration
      // by setting the timer to now
      t_expiry_max = now_time;
    }

    if (t_expiry_min > t_expiry_max)
      t_expiry_min = t_expiry_max;

    timer->expires = t_expiry_max;

    if (t_expiry_max == now_time) {
      // immediate re-registration
      DBG("calculated re-registration at TS <now> (%ld)"
	  "(reg_start_ts=%ld, reg_expiry=%ld, reregister_interval=%f, "
	  "minimum_reregister_interval=%f)\n",
	  t_expiry_max, reg_start_ts, expiry,
	  reregister_interval, minimum_reregister_interval);
      registration_scheduler.insert_timer(timer);
    } else {
      DBG("calculated re-registration at TS %ld .. %ld"
	  "(reg_start_ts=%ld, reg_expiry=%ld, reregister_interval=%f, "
	  "minimum_reregister_interval=%f)\n",
	  t_expiry_min, t_expiry_max, reg_start_ts, expiry,
	  reregister_interval, minimum_reregister_interval);
  
      registration_scheduler.insert_timer_leastloaded(timer, t_expiry_min, t_expiry_max);
    }
  } else {
    time_t t_expiry = reg_start_ts;
    if (expiry > reg_start_ts)
      t_expiry+=(expiry - reg_start_ts) * reregister_interval;

    if (t_expiry < now_time) {
      t_expiry = now_time;
      DBG("re-registering at TS <now> (%ld)\n", now_time);
    }

    DBG("calculated re-registration at TS %ld "
	"(reg_start_ts=%ld, reg_expiry=%ld, reregister_interval=%f)\n",
	t_expiry, reg_start_ts, expiry, reregister_interval);

    timer->expires = t_expiry;    
    registration_scheduler.insert_timer(timer);
  }
}

void DBRegAgent::clearRegistrationTimer(long object_id, const string& type) {
  DBG("Removing timer for subscription %ld, type: %s", object_id, type.c_str());

  map<long, RegTimer*>::iterator it;

  // clear registration timer for peerings
  if (type == TYPE_PEERING) {
    it=registration_timers_peers.find(object_id);
    if (it==registration_timers_peers.end()) {
      DBG("timer object for subscription %ld not found, type: %s\n", object_id, type.c_str());
      return;
    }
  // clear registration timer for subscribers
  } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
    it=registration_timers.find(object_id);
    if (it==registration_timers.end()) {
      DBG("timer object for subscription %ld not found, type: %s\n", object_id, type.c_str());
      return;
    }
  }

  DBG("removing timer [%p] from scheduler\n", it->second);
  registration_scheduler.remove_timer(it->second);

  DBG("deleting timer object [%p]\n", it->second);
  delete it->second;

  if (type == TYPE_PEERING)
    registration_timers_peers.erase(it);
  else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED)
    registration_timers.erase(it);
}

void DBRegAgent::removeRegistrationTimer(long object_id, const string& type) {
  DBG("removing timer object for subscription %ld, type: %s", object_id, type.c_str());

  map<long, RegTimer*>::iterator it;

  // remove registration timer for peerings
  if (type == TYPE_PEERING) {
    it=registration_timers_peers.find(object_id);
    if (it==registration_timers_peers.end()) {
      DBG("timer object for subscription %ld not found, type: %s\n", object_id, type.c_str());
      return;
    }

  // remove registration timer for subscribers
  } else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED) {
    it=registration_timers.find(object_id);
    if (it==registration_timers.end()) {
      DBG("timer object for subscription %ld not found, type: %s\n", object_id, type.c_str());
      return;
    }
  }

  DBG("deleting timer object [%p]\n", it->second);
  delete it->second;

  if (type == TYPE_PEERING)
    registration_timers_peers.erase(it);
  else if (type == TYPE_SUBSCRIBER || type == TYPE_UNDEFINED)
    registration_timers.erase(it);
}

void DBRegAgent::timer_cb(RegTimer* timer, long object_id, int reg_action, const string& type) {
  DBG("re-registration timer expired: subscriber %ld, timer=[%p], action %d, type %s\n",
      object_id, timer, reg_action, type.c_str());

  registrations_mut.lock();
  removeRegistrationTimer(object_id, type);
  registrations_mut.unlock();

  switch (reg_action) {
  case RegistrationActionEvent::Register:
    scheduleRegistration(object_id, type); break;
  case RegistrationActionEvent::Deregister:
    scheduleDeregistration(object_id, type); break;
  default: ERROR("internal: unknown reg_action %d for subscriber %ld timer event\n",
		 reg_action, object_id);
  };
}

void DBRegAgent::DIcreateRegistration(int object_id, const string& user,
              const string& pass, const string& realm,
              const string& contact, const string& auth_user,
              const string& type, AmArg& ret) {

  string auth_user_temp = (auth_user.empty() || auth_user == "" || auth_user == "NULL") ? user : auth_user;

  DBG("DI method: createRegistration(%i, %s, %s, %s, %s, %s)\n",
      object_id, auth_user_temp.c_str(), user.c_str(),
      pass.c_str(), realm.c_str(), contact.c_str());

  createRegistration(object_id, auth_user_temp, user, pass, realm, contact, type);
  scheduleRegistration(object_id, type);
  ret.push(200);
  ret.push("OK");
}

void DBRegAgent::DIupdateRegistration(int object_id, const string& user,
              const string& pass, const string& realm,
              const string& contact, const string& auth_user,
              const string& type, AmArg& ret) {

  string auth_user_temp = (auth_user.empty() || auth_user == "" || auth_user == "NULL") ? user : auth_user;

  DBG("DI method: updateRegistration(%i, %s, %s, %s, %s)\n",
      object_id, auth_user_temp.c_str(), user.c_str(),
      pass.c_str(), realm.c_str());

  string contact_uri = contact;
  if (contact_uri.empty() && !contact_hostport.empty()) {
    contact_uri = "sip:"+ user + "@" + contact_hostport;
  }

  updateRegistration(object_id, auth_user_temp, user, pass, realm, contact_uri, type);

  ret.push(200);
  ret.push("OK");
}

void DBRegAgent::DIremoveRegistration(int object_id, const string& type, AmArg& ret) {
  DBG("DI method: removeRegistration(%i)\n",
      object_id);
  scheduleDeregistration(object_id, type);

  registrations_mut.lock();
  clearRegistrationTimer(object_id, type);
  registrations_mut.unlock();

  ret.push(200);
  ret.push("OK");
}

void DBRegAgent::DIrefreshRegistration(int object_id, const string& type, AmArg& ret) {
  DBG("DI method: refreshRegistration(%i)\n", object_id);
  scheduleRegistration(object_id, type);

  ret.push(200);
  ret.push("OK");
}

// ///////// DI API ///////////////////

void DBRegAgent::invoke(const string& method,
			const AmArg& args, AmArg& ret)
{
  // create a brand new registration
  if (method == "createRegistration"){
    args.assertArrayFmt("issssss"); // object_id, user, pass, realm, contact, auth_user, type
    string contact;
    string auth_user;
		string type;                   // 'peering' or 'subscriber'

    // for case when: object_id, user, pass, realm, contact
    if (args.size() == 5) {
      assertArgCStr(args.get(4));
      contact = args.get(4).asCStr();

    // for case when: object_id, user, pass, realm, contact, auth_user
    } else if (args.size() == 6) {
      assertArgCStr(args.get(4));
      assertArgCStr(args.get(5));
      contact = args.get(4).asCStr();
      auth_user = args.get(5).asCStr();

    // for case when: object_id, user, pass, realm, contact, auth_user, type
    } else if (args.size() == 7) {
      assertArgCStr(args.get(4));
      assertArgCStr(args.get(5));
      assertArgCStr(args.get(6));
      contact = args.get(4).asCStr();
      auth_user = args.get(5).asCStr();
      type = args.get(6).asCStr();
    }

    // we only allow three possible types: 'peering', 'subscriber' and 'undefined'
    if (type.empty() || (type != TYPE_PEERING && type != TYPE_SUBSCRIBER && type != TYPE_UNDEFINED)) {
      DBG("REGISTER: Wrong type of the registration object defined: <%s>. Trying to fix.\n", type.c_str());
      type = TYPE_UNDEFINED;
    }

    DBG("REGISTER: SEMS is about to Create a registration for: object_id=<%d>, type=<%s>, user=<%s>, realm=<%s> \n",
      args.get(0).asInt(), type.c_str(), args.get(1).asCStr(), args.get(3).asCStr());
    DIcreateRegistration(args.get(0).asInt(), args.get(1).asCStr(),
        args.get(2).asCStr(), args.get(3).asCStr(), contact, auth_user, type, ret);

  // update an existing registration
  } else if (method == "updateRegistration"){
    args.assertArrayFmt("issssss"); // object_id, user, pass, realm, contact, auth_user, type
    string contact;
    string auth_user;
    string type;                   // 'peering' or 'subscriber'

    // for case when: object_id, user, pass, realm, contact
    if (args.size() == 5) {
      assertArgCStr(args.get(4));
      contact = args.get(4).asCStr();

    // for case when: object_id, user, pass, realm, contact, auth_user
    } else if (args.size() == 6) {
      assertArgCStr(args.get(4));
      assertArgCStr(args.get(5));
      contact = args.get(4).asCStr();
      auth_user = args.get(5).asCStr();

    // for case when: object_id, user, pass, realm, contact, auth_user, type
    } else if (args.size() == 7) {
      assertArgCStr(args.get(4));
      assertArgCStr(args.get(5));
      assertArgCStr(args.get(6));
      contact = args.get(4).asCStr();
      auth_user = args.get(5).asCStr();
      type = args.get(6).asCStr();
    }

    // we only allow three possible types: 'peering', 'subscriber' and 'undefined'
    if (type.empty() || (type != TYPE_PEERING && type != TYPE_SUBSCRIBER && type != TYPE_UNDEFINED)) {
      DBG("REGISTER: Wrong type of the registration object defined: <%s>. Trying to fix.\n", type.c_str());
      type = TYPE_UNDEFINED;
    }

    DBG("REGISTER: SEMS is about to Update a registration for: object_id=<%d>, type=<%s>, user=<%s>, realm=<%s> \n",
      args.get(0).asInt(), type.c_str(), args.get(1).asCStr(), args.get(3).asCStr());
    DIupdateRegistration(args.get(0).asInt(), args.get(1).asCStr(),
			 args.get(2).asCStr(), args.get(3).asCStr(), contact, auth_user, type, ret);

  // remove an existing registration
  } else if (method == "removeRegistration") {
    args.assertArrayFmt("is"); // object_id, type
    string type;               // must be 'peering' or 'subscriber'
    if (args.size() == 2) type = args.get(1).asCStr();
    if (type.empty()) type = TYPE_UNDEFINED;
    DIremoveRegistration(args.get(0).asInt(), type, ret);

  // refresh an existing registration
  } else if (method == "refreshRegistration") {
    args.assertArrayFmt("is"); // object_id, type
    string type;               // must be 'peering' or 'subscriber'
    if (args.size() == 2) type = args.get(1).asCStr();
    if (type.empty()) type = TYPE_UNDEFINED;
    DIrefreshRegistration(args.get(0).asInt(), type, ret);

  }  else if(method == "_list") {
    ret.push(AmArg("createRegistration"));
    ret.push(AmArg("updateRegistration"));
    ret.push(AmArg("removeRegistration"));
    ret.push(AmArg("refreshRegistration"));
  } else {
    throw AmDynInvoke::NotImplemented(method);
  }
}

// /////////////// processor thread /////////////////

DBRegAgentProcessorThread::DBRegAgentProcessorThread()
  : AmEventQueue(this), stopped(false) {
}

DBRegAgentProcessorThread::~DBRegAgentProcessorThread() {
}

void DBRegAgentProcessorThread::on_stop() {
}

void DBRegAgentProcessorThread::rateLimitWait() {
  DBG("applying rate limit %u initial requests per %us\n",
      DBRegAgent::ratelimit_rate, DBRegAgent::ratelimit_per);

  DBG("allowance before ratelimit: %f\n", allowance);

  struct timeval current;
  struct timeval time_passed;
  gettimeofday(&current, 0);
  timersub(&current, &last_check, &time_passed);
  memcpy(&last_check, &current, sizeof(struct timeval));
  double seconds_passed = (double)time_passed.tv_sec +
    (double)time_passed.tv_usec / 1000000.0;
  allowance += seconds_passed * 
    (double) DBRegAgent::ratelimit_rate / (double)DBRegAgent::ratelimit_per;

  if (allowance > (double)DBRegAgent::ratelimit_rate)
    allowance = (double)DBRegAgent::ratelimit_rate; // enough time passed, but limit to max
  if (allowance < 1.0) {
    useconds_t sleep_time = 1000000.0 * (1.0 - allowance) *
      ((double)DBRegAgent::ratelimit_per/(double)DBRegAgent::ratelimit_rate);
    DBG("not enough allowance (%f), sleeping %d useconds\n", allowance, sleep_time);
    usleep(sleep_time);
    allowance=0.0;
    gettimeofday(&last_check, 0);
  } else {
    allowance -= 1.0;
  }

  DBG("allowance left: %f\n", allowance);
}

void DBRegAgentProcessorThread::run() {
  DBG("DBRegAgentProcessorThread thread started\n");
  
  // register us as SIP event receiver for MOD_NAME_processor
  AmEventDispatcher::instance()->addEventQueue(MOD_NAME "_processor",this);

  mysqlpp::Connection::thread_start();

  // initialize ratelimit
  gettimeofday(&last_check, NULL);
  if (DBRegAgent::ratelimit_slowstart)
    allowance = 0.0;
  else
    allowance = DBRegAgent::ratelimit_rate;

  reg_agent = DBRegAgent::instance();
  while (!stopped) {
    waitForEvent();
    while (eventPending()) {
      rateLimitWait();
      processSingleEvent();
    }
  }

  mysqlpp::Connection::thread_end();

 DBG("DBRegAgentProcessorThread thread stopped\n"); 
}

void DBRegAgentProcessorThread::process(AmEvent* ev) {

  if (ev->event_id == E_SYSTEM) {
    AmSystemEvent* sys_ev = dynamic_cast<AmSystemEvent*>(ev);
    if(sys_ev){	
      DBG("Session received system Event\n");
      if (sys_ev->sys_event == AmSystemEvent::ServerShutdown) {
	DBG("stopping processor thread\n");
	stopped = true;
      }
      return;
    }
  }


  if (ev->event_id == RegistrationActionEventID) {
    RegistrationActionEvent* reg_action_ev =
      dynamic_cast<RegistrationActionEvent*>(ev);
    if (reg_action_ev) {
      reg_agent->onRegistrationActionEvent(reg_action_ev);
      return;
    }
  }

  ERROR("unknown event received!\n");
}
#if 0
void test_cb(RegTimer* tr, long data1, void* data2) {
  DBG("cb called: [%p], data %ld / [%p]\n", tr, data1, data2);
}

void DBRegAgent::run_tests() {

  registration_timer.start();

  struct timeval now;
  gettimeofday(&now, 0);

  RegTimer rt;
  rt.expires = now.tv_sec + 10; 
  rt.cb=test_cb;
  registration_scheduler.insert_timer(&rt);

  RegTimer rt2;
  rt2.expires = now.tv_sec + 5; 
  rt2.cb=test_cb;
  registration_scheduler.insert_timer(&rt2);

  RegTimer rt3;
  rt3.expires = now.tv_sec + 15; 
  rt3.cb=test_cb;
  registration_scheduler.insert_timer(&rt3);

  RegTimer rt4;
  rt4.expires = now.tv_sec - 1; 
  rt4.cb=test_cb;
  registration_scheduler.insert_timer(&rt4);

  RegTimer rt5;
  rt5.expires = now.tv_sec + 100000; 
  rt5.cb=test_cb;
  registration_scheduler.insert_timer(&rt5);

  RegTimer rt6;
  rt6.expires = now.tv_sec + 100; 
  rt6.cb=test_cb;
  registration_scheduler.insert_timer_leastloaded(&rt6, now.tv_sec+5, now.tv_sec+50);


  sleep(30);
  gettimeofday(&now, 0);

  RegTimer rt7;
  rt6.expires = now.tv_sec + 980; 
  rt6.cb=test_cb;
  registration_scheduler.insert_timer_leastloaded(&rt6, now.tv_sec+9980, now.tv_sec+9990);

   vector<RegTimer*> rts;

   for (int i=0;i<1000;i++) {
     RegTimer* t = new RegTimer();
     rts.push_back(t);
     t->expires = now.tv_sec + i;
     t->cb=test_cb;
     registration_scheduler.insert_timer_leastloaded(t, now.tv_sec, now.tv_sec+1000);
   }

  sleep(200);
}
#endif
