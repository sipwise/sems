/*
 * Kirill Solomko @ Sipwise GmbH
 */

#include "AOC.h"
#include "AmUtils.h"
#include "AmSipHeaders.h"
#include <math.h>
#include <sstream>

#define ITOS(x) static_cast<std::ostringstream&>((std::ostringstream() << std::dec << x)).str()
#define DTOS(x) static_cast<std::ostringstream&>((std::ostringstream() << std::dec << x)).str()

EXPORT_SESSION_EVENT_HANDLER_FACTORY(AOCFactory, MOD_NAME);

int AOCFactory::onLoad()
{
  INFO("AOC: onLoad triggerred");
  return 0;
}

bool AOCFactory::onInvite(const AmSipRequest& req, AmConfigReader& cfg)
{
  INFO("AOC: onInvite triggerred");
  return true;
}


AmSessionEventHandler* AOCFactory::getHandler(AmSession* s)
{
  INFO("AOC: getHandler triggerred");
  return new AOC(s);
}

#ifdef WITH_REPLICATION
AmSessionEventHandler* AOCFactory::getHandler(AmSession* s,
						       const string& state) {
  INFO("AOC: getHandler replicated triggerred");
  AOC* res = new AOC(s);
  if (res->restore(state))
    return res;
  ERROR("Restoring AOC failed from state '%s'\n",
	state.c_str());
  delete res;
  return NULL;
}
#endif

AOC::AOC(AmSession* s)
  :AmSessionEventHandler(),
   s(s),
   aoc_received(false),
   aoc_active(false),
   amount(0),
   multiplier(0),
   interval(0),
   setup(0),
   setup_charged(false),
   setup_interval(0.01),
   setup_count(0),
   free_interval(0),
   free_charged(false),
   count(0),
   error("")
{
}

bool AOC::process(AmEvent* ev)
{
  if (!aoc_conf.getEnableAOC())
    return false;

  INFO("AOC: process triggerred");

  assert(ev);
  AmTimeoutEvent* timeout_ev = dynamic_cast<AmTimeoutEvent*>(ev);
  if (timeout_ev) {
    int timer_id = timeout_ev->data.get(0).asInt();
    //INFO("AOC: checking timer with id %d\n", timer_id);
    if (timer_id == ID_AOC_CURRENCY_TIMER ||
        timer_id == ID_AOC_PULSE_TIMER ||
        timer_id == ID_AOC_PULSE_SETUP_TIMER ||
        timer_id == ID_AOC_FREE_TIMER) {
      DBG("received timeout Event with ID %d\n", timer_id);
      onTimeoutEvent(timeout_ev);
      return true;
    }
  }

  return false;
}

bool AOC::onSipRequest(const AmSipRequest& req)
{
  if (!aoc_conf.getEnableAOC())
    return false;

  INFO("AOC: onSipRequest %s\n", req.method.c_str());

  if (req.method == "INVITE" && !aoc_received) {
    string aoc_header = getHeader(req.hdrs, INT_AOC_HEADER, true);
    if (!aoc_header.empty()) {
      vector<string> params = explode(aoc_header, ";", 1);
      INFO("AOC: header detected %s\n", aoc_header.c_str());
      bool invalid = false;
      for (vector<string>::const_iterator i = params.begin(); i != params.end(); ++i) {
        if (!i->empty()) {
          int pos = static_cast<int>(i->find("="));
          if (pos > 0) {
            string key = i->substr(0,pos);
            string val = i->substr(pos+1);
            INFO("AOC: test %s - %s\n", key.c_str(), val.c_str());
            aoc_values[key] = val;
            if (key == "setup") {
              setup = atof(val.c_str());
              if (setup < 0)
                invalid = true;
            } else if (key == "free") {
              free_interval = atof(val.c_str());
              if (free_interval < 0)
                invalid = true;
            } else if (key == "amount") {
              amount = atof(val.c_str());
              if (amount < 0)
                invalid = true;
            } else if (key == "multiplier") {
              multiplier = atof(val.c_str());
              if (multiplier < 0)
                invalid = true;
            } else if (key == "interval") {
              interval = atof(val.c_str());
              if (interval < 0)
                invalid = true;
            } else if (key == "charging-info") {
              charging_info = val;
            } else if (key == "currency") {
              currency = val;
            } else if (key == "setup-interval") {
              setup_interval = atof(val.c_str());
              if (setup_interval < 0)
                invalid = true;
            }
            if (invalid)
              break;
          }
        }
      }
      if (charging_info.empty() || invalid) {
        WARN("AOC: Invalid header detected %s\n", aoc_header.c_str());
      } else {
        aoc_received = true;
      }
    }
  }

  if (req.method == "BYE") {
    removeTimers(s);
    return false;
  }

#ifdef WITH_REPLICATION
  s->setEventHandlerState(MOD_NAME, save());
#endif
  return false;
}

bool AOC::onSendReply(const AmSipRequest& req,
			       unsigned int  code,const string& reason,
			       const string& content_type,const string& body,
			       string& hdrs,
			       int flags)
{
  if (!aoc_conf.getEnableAOC())
    return false;

  INFO("AOC: onSendReply %s %d %s\n", req.method.c_str(), code, reason.c_str());

  if (aoc_received) {
    if (req.method == "INVITE" && code == 200) {
      aoc_active = true;
      INFO("AOC: active\n");
      string aoc_hdr_ct = "application/x";
      string aoc_hdrs = hdrs;
      aoc_hdrs += SIP_HDR_COLSP(SIP_HDR_CONTENT_TYPE) + aoc_hdr_ct + CRLF;
      aoc_request = SIPRequestInfo("INFO", "", "", aoc_hdrs);
      dispatchAOC();
    } else if (aoc_active && req.method == "INFO" && code == 488) {
      INFO("AOC: inactive\n");
      removeTimers(s);
      aoc_active = false;
    }
  }

  if (req.method == "BYE") {
    removeTimers(s);
  }

  return false;
}

bool AOC::onSendRequest(const string& method,
				 const string& content_type,
				 const string& body,
				 string& hdrs,
				 int flags,
				 unsigned int cseq)
{
  if (!aoc_conf.getEnableAOC())
    return false;

  INFO("AOC: onSendRequest %s\n", method.c_str());

  if (method == "INVITE")
    removeHeader(hdrs, INT_AOC_HEADER);

  return false;
}

bool AOC::onSipReply(const AmSipReply& reply, int old_dlg_status,
			               const string& trans_method)
{
  if (!aoc_conf.getEnableAOC())
    return false;

  INFO("AOC: onSipReply %s - %d\n", trans_method.c_str(), reply.code);

  return false;
}

int AOC::configure(AmConfigReader& conf)
{
  INFO("AOC: configure event\n");
  if (aoc_conf.readFromConfig(conf))
    return -1;

  INFO("AOC: enabled: %d\n", aoc_conf.getEnableAOC());

  return 0;
}

void AOC::removeTimers(AmSession* s)
{
  s->removeTimer(ID_AOC_CURRENCY_TIMER);
  s->removeTimer(ID_AOC_PULSE_TIMER);
  s->removeTimer(ID_AOC_PULSE_SETUP_TIMER);
  return;
}

void AOC::onTimeoutEvent(AmTimeoutEvent* timeout_ev)
{
  INFO("AOC: timeout trigger");
  int timer_id = timeout_ev->data.get(0).asInt();
  switch (timer_id) {
    case ID_AOC_FREE_TIMER:
      free_charged = true;
      dispatchAOC();
      break;
    case ID_AOC_CURRENCY_TIMER:
      if (!sendAOCCurrencyMessage())
        ERROR("%s: %s\n", MOD_NAME, error.c_str());
      break;
    case ID_AOC_PULSE_TIMER:
      if (!sendAOCPulseMessage())
        ERROR("%s: %s\n", MOD_NAME, error.c_str());
      break;
    case ID_AOC_PULSE_SETUP_TIMER:
      if (!sendAOCPulseSetupMessage())
        ERROR("%s: %s\n", MOD_NAME, error.c_str());
      break;
    default:
      ERROR("%s: %s %d\n", MOD_NAME,
        "Unknown AOC timer with id ", timer_id);
      break;
  }
  return;
}

void AOC::dispatchAOC()
{
  if (setup && !setup_count && !setup_charged) {
    // prepare and dispatch AOC setup messages if set
    if (charging_info == "currency")
      s->setTimer(ID_AOC_CURRENCY_TIMER, 1);
    else if (charging_info == "pulse")
      s->setTimer(ID_AOC_PULSE_SETUP_TIMER, 1);
  }

  if (free_interval && !free_charged) {
      INFO("AOC: free interval active: %f\n", free_interval);
      // delay AOC messages to be sent after the free interval
      s->setTimer(ID_AOC_FREE_TIMER, free_interval);
      return;
  }

  if (!setup || free_charged) {
    if (charging_info == "currency") {
      if (count == 0 && !free_interval)
        s->setTimer(ID_AOC_CURRENCY_TIMER, 1);
      else
        sendAOCCurrencyMessage();
    } else if (charging_info == "pulse") {
      if (count == 0 && !free_interval)
        s->setTimer(ID_AOC_PULSE_TIMER, 1);
      else
        sendAOCPulseMessage();
    }
  }

  return;
}

bool AOC::sendAOCCurrencyMessage()
{
  if (!aoc_active) {
    error = "There an error occured when sending the AOC currency message (AOC is inactive)";
    return false;
  }
  if (charging_info != "currency")
    return true;

  string aoc_hdrs = aoc_request.hdrs;
  string aoc_hdr_data;

  if (setup && !setup_charged && free_interval)
    count += 0;
  else
    count += 1;

  aoc_hdr_data += "type=active;charging;";
  aoc_hdr_data += "charging-info=" + charging_info + ";";
  if (setup && !setup_charged && free_interval)
    aoc_hdr_data += "amount=" + DTOS(setup) + ";";
  else
    aoc_hdr_data += "amount=" + DTOS(setup + (amount*multiplier*count*100)) + ";";
  aoc_hdr_data += "multiplier=" + DTOS(multiplier) + ";";
  aoc_hdr_data += "currency=" + currency + ";";

  if (aoc_hdr_data.empty()) {
    count -= 1;
    error = "There an error occured when sending the AOC pulse message (empty header data)";
    return false;
  }

  INFO("AOC: INFO currency message to the a-party: %s\n", aoc_hdr_data.c_str());
  aoc_hdrs += SIP_HDR_COLSP(AOC_HEADER) + aoc_hdr_data + CRLF;
  s->dlg.sendRequest(aoc_request.method,
                     aoc_request.content_type,
                     aoc_request.body,
                     aoc_hdrs);

  if (setup && !setup_charged)
    setup_charged = true;

  if (interval && (!free_interval || free_charged))
    s->setTimer(ID_AOC_CURRENCY_TIMER, interval);
  else
    s->removeTimer(ID_AOC_CURRENCY_TIMER);

  return true;
}

bool AOC::sendAOCPulseMessage()
{
  if (!aoc_active) {
    error = "There an error occured when sending the AOC pulse message (AOC is inactive)";
    return false;
  }
  if (charging_info != "pulse")
    return true;

  string aoc_hdrs = aoc_request.hdrs;
  string aoc_hdr_data;

  count += 1;
  aoc_hdr_data += "type=active;charging;";
  aoc_hdr_data += "charging-info=" + charging_info + ";";
  aoc_hdr_data += "recorded-units=" + ITOS(count) + ";";

  if (aoc_hdr_data.empty()) {
    count -= 1;
    error = "There an error occured when sending the AOC pulse message (empty header data)";
    return false;
  }

  INFO("AOC: sending INFO pulse message to the a-party: %s\n", aoc_hdr_data.c_str());
  aoc_hdrs += SIP_HDR_COLSP(AOC_HEADER) + aoc_hdr_data + CRLF;
  s->dlg.sendRequest(aoc_request.method,
                     aoc_request.content_type,
                     aoc_request.body,
                     aoc_hdrs);
  
  if (interval)
    s->setTimer(ID_AOC_PULSE_TIMER, interval);
  else
    s->removeTimer(ID_AOC_PULSE_TIMER);

  return true;
}

bool AOC::sendAOCPulseSetupMessage()
{
  if (!aoc_active) {
    error = "There an error occured when sending the AOC pulse setup message (AOC is inactive)";
    return false;
  }
  if (charging_info != "pulse")
    return true;
  if (setup <= 0 || setup_charged)
    return true;
  if (amount <= 0) {
    error = "There an error occured when sending the AOC pulse setup message (amount is less or equal 0)";
    return false;
  }

  string aoc_hdrs = aoc_request.hdrs;
  string aoc_hdr_data;

  count += 1;

  if (setup && !setup_count && !setup_charged)
    setup_count = ceil(setup / amount);

  aoc_hdr_data += "type=active;charging;";
  aoc_hdr_data += "charging-info=" + charging_info + ";";
  aoc_hdr_data += "recorded-units=" + ITOS(count) + ";";

  if (aoc_hdr_data.empty()) {
    count -= 1;
    error = "There an error occured when sending the AOC pulse message (empty header data)";
    return false;
  }

  INFO("AOC: sending INFO pulse setup message to the a-party: %s\n", aoc_hdr_data.c_str());
  aoc_hdrs += SIP_HDR_COLSP(AOC_HEADER) + aoc_hdr_data + CRLF;
  s->dlg.sendRequest(aoc_request.method,
                     aoc_request.content_type,
                     aoc_request.body,
                     aoc_hdrs);

  setup_count -= 1;

  if (setup && !setup_charged && setup_count <= 0) {
    setup_charged = true;
    s->removeTimer(ID_AOC_PULSE_SETUP_TIMER);
    if (!free_interval || free_charged)
      s->setTimer(ID_AOC_PULSE_TIMER, setup_interval);
  } else if (setup_interval)
    s->setTimer(ID_AOC_PULSE_SETUP_TIMER, setup_interval);
  else
    s->removeTimer(ID_AOC_PULSE_SETUP_TIMER);

  return true;
}

// strips all trailing zeroes and removes '.' for ints
string AOC::double2string(double val)
{
  char buffer[32];
  if (trunc(val) == val)
    sprintf(buffer, "%d", (int)val);
  else
    sprintf(buffer, "%#.16g", val);
  char* ch = buffer + strlen(buffer) - 1;
  if (*ch != '0') return buffer; // nothing to truncate, so save time
  while(ch > buffer && *ch == '0'){
    --ch;
  }
  char* last_nonzero = ch;
  while(ch >= buffer){
    switch(*ch){
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      --ch;
      continue;
    case '.':
      *(last_nonzero+1) = '\0';
      return string(buffer);
    default:
      return string(buffer);
    }
  }
  return string(buffer);
}

AmAOCConfig::AmAOCConfig()
  : enableAOC(DEFAULT_ENABLE_AOC)
{
}

AmAOCConfig::~AmAOCConfig()
{
}

int AmAOCConfig::readFromConfig(AmConfigReader& cfg)
{
  // enable_aoc
  if (cfg.hasParameter("enable_aoc")){
    if (!setEnableAOC(cfg.getParameter("enable_aoc"))){
      ERROR("invalid or missing enable_aoc parameter\n");
      return -1;
    }
  }

  return 0;
}

int AmAOCConfig::setEnableAOC(const string& enable)
{
  if (strcasecmp(enable.c_str(), "yes") == 0 ) {
    enableAOC = 1;
  } else if (strcasecmp(enable.c_str(), "no") == 0 ) {
    enableAOC = 0;
  } else {
    return 0;
  }
  return 1;
}

#ifdef WITH_REPLICATION
#define TO_NETSTRING(s) int2str((s).length())+":"+s+","
#define INT_TO_STRCOL(u) int2str((u))+":"
bool st_skipto_colon(const string& s, size_t& p) {
  while (p<s.length() && s[p]!=':')
    p++;
  return p<s.length();
}

bool st_skip_int_col(const string& s, size_t& p, int& val) {
  size_t start = p;

  if (!st_skipto_colon(s, p)) // no colon found
    return false;

  if (p==start) // colon is at pos 0
    return false;

  // length can not be deciphered
  if (!str2int(s.substr(start, p-start), val))
    return false;

  p++; // skip trailing :

  return true;
}

bool AOC::restore(const string& state) {
  return true;
}

string AOC::save() {
  return "";
}

#endif
