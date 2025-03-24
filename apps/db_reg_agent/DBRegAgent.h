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

#ifndef _DB_REGAgent_h_
#define _DB_REGAgent_h_
#include <sys/time.h>

#include <mysql++/mysql++.h>


#include <map>
using std::map;
#include <queue>
using std::queue;

#include "AmApi.h"
#include "AmSipRegistration.h"

#include "RegistrationTimer.h"

#define REG_STATUS_INACTIVE      0
#define REG_STATUS_PENDING       1
#define REG_STATUS_ACTIVE        2
#define REG_STATUS_FAILED        3
#define REG_STATUS_REMOVED       4
#define REG_STATUS_TO_BE_REMOVED 5

#define REG_STATUS_INACTIVE_S      "0"
#define REG_STATUS_PENDING_S       "1"
#define REG_STATUS_ACTIVE_S        "2"
#define REG_STATUS_FAILED_S        "3"
#define REG_STATUS_REMOVED_S       "4"
#define REG_STATUS_TO_BE_REMOVED_S "5"

#define COLNAME_SUBSCRIBER_ID    "subscriber_id"
#define COLNAME_PEER_ID          "peer_host_id"
#define COLNAME_AUTH_USER        "auth_user"
#define COLNAME_USER             "user"
#define COLNAME_PASS             "pass"
#define COLNAME_REALM            "realm"
#define COLNAME_CONTACT          "contact"

#define COLNAME_STATUS           "registration_status"
#define COLNAME_EXPIRY           "expiry"
#define COLNAME_REGISTRATION_TS  "last_registration"
#define COLNAME_LAST_CODE        "last_code"
#define COLNAME_LAST_REASON      "last_reason"
#define COLNAME_ID_PK            "id"

#define TYPE_PEERING_STR             "peering"
#define TYPE_SUBSCRIBER_STR          "subscriber"

enum regType { TYPE_UNDEFINED=0, TYPE_PEERING, TYPE_SUBSCRIBER };

#define RegistrationActionEventID 117

#define ERR_REASON_UNABLE_TO_SEND_REQUEST  "unable to send request"

#define TYPE_TO_STRING(type) ((type) == TYPE_PEERING ? "peering" : "subscriber")

struct RegistrationActionEvent : public AmEvent {

  enum RegAction { Register=0, Deregister, Unknown };

RegistrationActionEvent(RegAction action, long object_id, const regType type)
  : AmEvent(RegistrationActionEventID),
    action(action), object_id(object_id), type(type) { }

  RegAction action;
  long object_id;
  const regType type;
};

class DBRegAgent;

// separate thread for REGISTER sending, which can block for rate limiting
class DBRegAgentProcessorThread
: public AmEventQueue,
  public AmThread,
  public AmEventHandler
{

  DBRegAgent* reg_agent;

  void rateLimitWait();

  double allowance;
  struct timeval last_check;

 protected:
  void process(AmEvent* ev);

 public:
  DBRegAgentProcessorThread();
  ~DBRegAgentProcessorThread();

  void run();
  void on_stop();

};

class RegTimer;

class DBRegAgent
: public AmDynInvokeFactory,
  public AmDynInvoke,
  public AmEventQueue,
  public AmThread,
  public AmEventHandler
{

  static string joined_query_subscribers;
  static string joined_query_peerings;
  static string registrations_table;

  static double reregister_interval;
  static double minimum_reregister_interval;

  static bool enable_ratelimiting;
  static unsigned int ratelimit_rate;
  static unsigned int ratelimit_per;
  static bool ratelimit_slowstart;

  static bool delete_removed_registrations;
  static bool delete_failed_deregistrations;
  static bool save_contacts;

  static bool db_read_contact;

  static string contact_hostport;

  static bool username_with_domain;

  static string outbound_proxy;

  static bool save_auth_replies;

  static unsigned int error_retry_interval;

  map<long, AmSIPRegistration*> registrations;              // usual subscribers
  map<string, long>             registration_ltags;
  map<long, RegTimer*>          registration_timers;

  map<long, AmSIPRegistration*> registrations_peers;        // SIP peerings
  map<string, long>             registration_ltags_peers;
  map<long, RegTimer*>          registration_timers_peers;

  AmMutex registrations_mut;

  // connection used in main DBRegAgent thread
  static mysqlpp::Connection MainDBConnection;

  // connection used in other thread (processor thread)
  static mysqlpp::Connection ProcessorDBConnection;

  int onLoad();

  // atomic_ref_cnt interface
  void on_destroy() {
    onUnload();
  }

  void onUnload();

  RegistrationTimer registration_scheduler;
  DBRegAgentProcessorThread registration_processor;

  bool loadRegistrations();         // for loading subscribers
  bool loadRegistrationsPeerings(); // for loading peerings

  void createDBRegistration(long object_id, const regType type, mysqlpp::Connection& conn);
  void deleteDBRegistration(long object_id, const regType type, mysqlpp::Connection& conn);
  void updateDBRegistration(mysqlpp::Connection& db_connection,
			    long object_id, const regType type, int last_code,
			    const string& last_reason,
			    bool update_status = false, int status = 0,
			    bool update_ts=false, unsigned int expiry = 0,
			    bool update_contacts=false, const string& contacts = "");

  /** create registration in our list */
    void createRegistration(long object_id,
        const string& auth_user,
        const string& user,
        const string& pass,
        const string& realm,
        const string& contact,
        const regType type);
  /** update registration in our list */
  void updateRegistration(long subscriber_id,
        const string& auth_user,
        const string& user,
        const string& pass,
        const string& realm,
        const string& contact,
        const regType type);

  /** remove registration */
  void removeRegistration(long object_id, const regType type);

  /** schedule this subscriber to REGISTER imminently */
  void scheduleRegistration(long object_id, const regType type);

  /** schedule this subscriber to de-REGISTER imminently*/
  void scheduleDeregistration(long object_id, const regType type);

  /** create a timer for the registration - fixed expiry + action */
  void setRegistrationTimer(long object_id, uint64_t timeout,
			    RegistrationActionEvent::RegAction reg_action, const regType type);

  /** create a registration refresh timer for that registration 
      @param object_id     - ID of subscription
      @param expiry        - SIP registration expiry time
      @param reg_start_ts  - start TS of the SIP registration
      @param now_time      - current time
   */
  void setRegistrationTimer(long object_id,
			    uint64_t expiry, uint64_t reg_start_ts, uint64_t now_time, const regType type);

  /** clear re-registration timer and remove timer object */
  void clearRegistrationTimer(long object_id, const regType type);

  /** remove timer object */
  void removeRegistrationTimer(long object_id, const regType type);
  
  /** handle timer object */
  void handleRegistrationTimer(long object_id, const regType type);

  /** handle registration scheduling */
  void handleRegistrationScheduling(long object_id, const std::string &auth_user,
                                    const std::string &user, const std::string &pass,
                                    const std::string &realm, const std::string &contact,
                                    const regType type);

  //  void run_tests();

  // amThread
  void run();
  void on_stop();

  // AmEventHandler
  void process(AmEvent* ev);

  void onSipReplyEvent(AmSipReplyEvent* ev);

  void onRegistrationActionEvent(RegistrationActionEvent* reg_action_ev);


  unsigned int expires;

  AmDynInvoke* uac_auth_i;

  void DIcreateRegistration(int object_id, const string& user,
          const string& pass, const string& realm,
          const string& contact, const string& auth_user, const regType type, AmArg& ret);
  void DIupdateRegistration(int object_id, const string& user,
          const string& pass, const string& realm,
          const string& contact, const string& auth_user, const regType type, AmArg& ret);
  void DIremoveRegistration(int object_id, const regType type, AmArg& ret);
  void DIrefreshRegistration(int object_id, const regType type, AmArg& ret);


 public:
  DBRegAgent(const string& _app_name);
  ~DBRegAgent();

  DECLARE_MODULE_INSTANCE(DBRegAgent);

  // DI
  // DI factory
  AmDynInvoke* getInstance() { return instance(); }
  // DI API
  void invoke(const string& method, 
	      const AmArg& args, AmArg& ret);
  /** re-registration timer callback */
  void timer_cb(RegTimer* timer);

  friend class DBRegAgentProcessorThread;
};

#endif
