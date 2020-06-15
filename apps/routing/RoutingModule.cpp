#include "RoutingModule.h"
#include "Log.h"
#include "RoutingRule.h"

#include <AmPlugIn.h>
#include <AmArg.h>

#include <SBCCallControlAPI.h>
#include <SBCCallLeg.h>
#include <SipCtrlInterface.h>
#include <string.h>
#include <boost/regex.hpp>

#define MAX_RULES 100

namespace routing
{

EXPORT_PLUGIN_CLASS_FACTORY(RoutingModuleFactory, MOD_NAME);

RoutingModule* RoutingModule::_instance = 0;

RoutingModule* RoutingModule::instance()
{
    if(!_instance)
    {
        _instance = new RoutingModule();
    }

    return _instance;
}

RoutingModule::RoutingModule() { }

RoutingModule::~RoutingModule() { }

int RoutingModule::onLoad()
{
    AmConfigReader cfg;

    if(cfg.loadFile(AmConfig::ModConfigPath + std::string(MOD_NAME ".conf")))
    {
        LogInfo(<< MOD_NAME << " configuration file (" << (AmConfig::ModConfigPath + std::string(MOD_NAME ".conf")) << ") not found, assuming default configuration is fine");
    }

    // load routing rules
    int i = 0;
    std::string index;
    do
    {
        RoutingRule r;
        r.type = cfg.getParameter("type" + index, "");
        r.match = cfg.getParameter("match" + index, "");
        r.destination = cfg.getParameter("destination" + index, "");
        r.app = cfg.getParameter("app" + index, "");

        if (!r.type.empty() && !r.match.empty())
        {
            mRoutingRules.push_back(r);
        }
        else
        {
            LogWarning(<< "invalid rule #" << index << ", skipped");
        }

        i++;

        index = int2str(i);
    }
    while (i < MAX_RULES);

    if (i <= 0)
    {
        LogWarning(<< "Routring rules is not provided for " << MOD_NAME << ". Routing modules is useless in this state, please consider disabling it.");
    }

    return 0;
}

void RoutingModule::invoke(const std::string & method, const AmArg & args, AmArg & ret)
{
    LogDebug(<< "RoutingModule: " << method << "(" << AmArg::print(args) << ")");

    if (method == "start" || method == "connect" || method == "end")
    {
    }
    else if (method == "getExtendedInterfaceHandler")
    {
       ret.push((AmObject*)this);
    }
    else if(method == "_list")
    {
        ret.push("start");
        ret.push("connect");
        ret.push("end");
        ret.push("list");
    }
    else
    {
        throw AmDynInvoke::NotImplemented(method);
    }
}

// ------- extended call control interface -------------------
bool RoutingModule::init(SBCCallLeg *call, const map<std::string, std::string> &values)
{
    return true;
}

CCChainProcessing RoutingModule::onInitialInvite(SBCCallLeg *call, AmSipRequest &req)
{
    return ContinueProcessing;
}

void RoutingModule::onStateChange(SBCCallLeg *call, const CallLeg::StatusChangeCause &cause)
{
};

CCChainProcessing RoutingModule::onBLegRefused(SBCCallLeg *call, const AmSipReply & reply)
{
    return ContinueProcessing;
}

void RoutingModule::onDestroyLeg(SBCCallLeg *call)
{
}

CCChainProcessing RoutingModule::onInDialogRequest(SBCCallLeg *call, const AmSipRequest &req)
{
    LogDebug(<< "onInDialogRequest");

    if (checkAndRoute(call, req))
    {
        LogDebug(<< "routing applied");
    }

    return ContinueProcessing;
}

CCChainProcessing RoutingModule::onInDialogReply(SBCCallLeg *call, const AmSipReply &reply)
{
    return ContinueProcessing;
}

void RoutingModule::onMatch(SBCCallLeg *call, const AmSipRequest & req, const RoutingRule & r)
{
    // route to destination if required
    if (!r.destination.empty())
    {
        call->getCallProfile().outbound_proxy = r.destination;
    }

    // route to sems app if required
    if (!r.app.empty())
    {
        std::string append_headers = std::string(APPNAME_HDR) + ": " + r.app + "\r\n";

        std::string params = getAppParams(r.app, req);

        if (!params.empty())
        {
            append_headers += std::string(PARAM_HDR) + ": " + params + "\r\n";
        }

        call->getCallProfile().append_headers = append_headers;
    }

}

std::string RoutingModule::getAppParams(const std::string & app, const AmSipRequest & req)
{
    return getHeader(req.hdrs, PARAM_HDR, true);
}

bool RoutingModule::checkAndRoute(SBCCallLeg *call, const AmSipRequest & req)
{
    LogDebug(<< "checkAndRoute");
    bool rc = false;

    for (RoutingRule &r: mRoutingRules)
    {
        if (r.type == "ruri_match")
        {
            rc = (r.match == req.r_uri);
        }
        else if (r.type == "ruri_regexp")
        {
            boost::regex expr{r.match};
            rc = boost::regex_match(req.r_uri, expr);
        }
        else if (r.type == "app_param_regexp")
        {
            boost::regex expr{r.match};
            std::string paramHeader = getHeader(req.hdrs, PARAM_HDR, true);
            rc = boost::regex_match(paramHeader, expr);
        }

        if (rc)
        {
            LogDebug(<< "routing rule applied: type: " << r.type << ", match: " << r.match);
            onMatch(call, req, r);
            break;
        }
    }

    return rc;
}

CCChainProcessing RoutingModule::onSendRequest(SBCCallLeg *call, const AmSipRequest & req)
{
    return ContinueProcessing;
}

CCChainProcessing RoutingModule::onEvent(SBCCallLeg *call, AmEvent *e)
{
    LogDebug(<< "ExtCC: onEvent - call instance: '" << call << "' isAleg==" << (call->isALeg() ? "true":"false"));


    return ContinueProcessing;
}

} // namsepace routing


