/*
 * Kirill Solomko @ Sipwise GmbH
 */

#ifndef AOC_H
#define AOC_H

#include "AmApi.h"
#include "AmSession.h"

#define MOD_NAME "aoc"

#define TIMER_OPTION_TAG  "timer"

#define INT_AOC_HEADER "P-App-AOC"
#define AOC_HEADER "AOC"

class AmTimeoutEvent;

#define ID_AOC_CURRENCY_TIMER 13371
#define ID_AOC_PULSE_TIMER 13372
#define ID_AOC_PULSE_SETUP_TIMER 13373
#define ID_AOC_FREE_TIMER 13374

/* AOC default configuration: */
#define DEFAULT_ENABLE_AOC 1

class AOCFactory: public AmSessionEventHandlerFactory
{

public:
  AOCFactory(const string& name) : AmSessionEventHandlerFactory(name) {}

  int onLoad();
  bool onInvite(const AmSipRequest& req, AmConfigReader& cfg);

  AmSessionEventHandler* getHandler(AmSession* s);

#ifdef WITH_REPLICATION
  AmSessionEventHandler* getHandler(AmSession* s, const string& state);
#endif
};

class AmAOCConfig
{

  friend class AOC;

  int enableAOC;

public:
  AmAOCConfig();
  ~AmAOCConfig();

  int readFromConfig(AmConfigReader& cfg);
  int setEnableAOC(const string& enable);
  bool getEnableAOC() { return enableAOC; }
};

struct SIPRequestInfo {
  string method;
  string content_type;
  string body;
  string hdrs;

  SIPRequestInfo(const string& method,
     const string& content_type,
     const string& body,
     const string& hdrs)
    : method(method), content_type(content_type),
       body(body), hdrs(hdrs) { }

  SIPRequestInfo() {}

};

/** \brief SessionEventHandler for implementing session timer logic for a session */
class AOC: public AmSessionEventHandler
{
  AmAOCConfig aoc_conf;
  AmSession* s;

  SIPRequestInfo aoc_request;
  std::map<string, string> aoc_values;

  bool aoc_received;
  bool aoc_active;
  bool setup_charged;
  bool free_charged;

  double amount;
  double multiplier;
  double setup;
  double interval;
  double setup_interval;
  double free_interval;

  int count;
  unsigned int setup_count;

  string charging_info;
  string currency;
  string error;

  string double2string(double d);

  bool sendAOCCurrencyMessage();
  bool sendAOCPulseMessage();
  bool sendAOCPulseSetupMessage();

  void dispatchAOC();

  void onTimeoutEvent(AmTimeoutEvent* timeout_ev);
  void removeTimers(AmSession* s);

  // string getReplyHeaders(const AmSipRequest& req);
  // string getRequestHeaders(const string& method);

 public:
  AOC(AmSession*);
  virtual ~AOC(){}

  virtual int  configure(AmConfigReader& conf);
  virtual bool process(AmEvent*);

  virtual bool onSipRequest(const AmSipRequest&);
  virtual bool onSipReply(const AmSipReply&, int old_dlg_status,
			  const string& trans_method);

  virtual bool onSendRequest(const string& method,
			     const string& content_type,
			     const string& body,
			     string& hdrs,
			     int flags,
			     unsigned int cseq);

  virtual bool onSendReply(const AmSipRequest& req,
			   unsigned int  code,
			   const string& reason,
			   const string& content_type,
			   const string& body,
			   string& hdrs,
			   int flags);

#ifdef WITH_REPLICATION
  bool restore(const string& state);
  string save();
#endif
};

#endif // AOC_H
