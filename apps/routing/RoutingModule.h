#pragma once

#include <AmApi.h>
#include <AmPlugIn.h>
#include <SBCCallLeg.h>
#include <SBCCallProfile.h>
#include <ExtendedCCInterface.h>
#include <vector>

namespace routing
{

class RoutingRule;

class RoutingModule : public AmObject, public AmDynInvoke, public ExtendedCCInterface
{
    static RoutingModule* _instance;

    void start(const std::string & cc_name, const std::string & ltag, SBCCallProfile* call_profile, int start_ts_sec, int start_ts_usec, const AmArg & values, int timer_id, AmArg & res);
    void connect(const std::string & cc_name, const std::string & ltag, SBCCallProfile* call_profile, const std::string & other_ltag, int connect_ts_sec, int connect_ts_usec);
    void end(const std::string & cc_name, const std::string & ltag, SBCCallProfile* call_profile, int end_ts_sec, int end_ts_usec);

public:
    RoutingModule();
    ~RoutingModule();

    static RoutingModule *instance();
    void invoke(const std::string & method, const AmArg & args, AmArg & ret);
    int onLoad();

    // extended call control interface
    // --- calls
    bool init(SBCCallLeg *call, const std::map<std::string, std::string> & values);
    CCChainProcessing onInitialInvite(SBCCallLeg *call, AmSipRequest & req);
    CCChainProcessing onBLegRefused(SBCCallLeg *call, const AmSipReply & reply);
    void onDestroyLeg(SBCCallLeg *call);
    void onStateChange(SBCCallLeg *call, const CallLeg::StatusChangeCause & cause);
    CCChainProcessing onInDialogRequest(SBCCallLeg *call, const AmSipRequest & req);
    CCChainProcessing onInDialogReply(SBCCallLeg *call, const AmSipReply & reply);
    CCChainProcessing onEvent(SBCCallLeg *call, AmEvent *e);
    CCChainProcessing onSendRequest(SBCCallLeg *call, const AmSipRequest & req);

private:

    std::string getAppParams(const std::string & app, const AmSipRequest & req);
    bool checkAndRoute(SBCCallLeg *call, const AmSipRequest & req);
    void onMatch(SBCCallLeg *call, const AmSipRequest & req, const RoutingRule & r);

    std::vector<RoutingRule> mRoutingRules;
};

class RoutingModuleFactory : public AmDynInvokeFactory
{
    public:
        RoutingModuleFactory(const std::string & name) : AmDynInvokeFactory(name) {}
        AmDynInvoke *getInstance() { return RoutingModule::instance(); }
        int onLoad() { return RoutingModule::instance()->onLoad(); }
};

} // namsepace routing


