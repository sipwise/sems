#ifndef _SW_VSC_H_
#define _SW_VSC_H_

#include "AmSession.h"
#include "AmConfigReader.h"
#include "AmAudioFile.h"
#include "AmUACAuth.h"

#include "ampi/UACAuthAPI.h"
#include <string>
using std::string;

#include <memory>
#include <regex.h>

#include <mysql.h>

typedef struct
{

    string mysqlHost;
    int mysqlPort;
    string mysqlUser;
    string mysqlPass;

    string audioPath;

    string failAnnouncement;
    string unknownAnnouncement;
    string voicemailNumber;

    regex_t cfuOnPattern;
    string cfuOnAnnouncement;

    regex_t cfuOffPattern;
    string cfuOffAnnouncement;

    regex_t cfbOnPattern;
    string cfbOnAnnouncement;

    regex_t cfbOffPattern;
    string cfbOffAnnouncement;

    regex_t cftOnPattern;
    string cftOnAnnouncement;

    regex_t cftOffPattern;
    string cftOffAnnouncement;

    regex_t cfnaOnPattern;
    string cfnaOnAnnouncement;

    regex_t cfnaOffPattern;
    string cfnaOffAnnouncement;

    regex_t speedDialPattern;
    string speedDialAnnouncement;

    regex_t reminderOnPattern;
    string reminderOnAnnouncement;

    regex_t reminderOffPattern;
    string reminderOffAnnouncement;

    regex_t blockinclirOnPattern;
    string blockinclirOnAnnouncement;

    regex_t blockinclirOffPattern;
    string blockinclirOffAnnouncement;
} sw_vsc_patterns_t;

class SW_VscFactory: public AmSessionFactory
{
    inline string getAnnounceFile(const AmSipRequest &req);

    sw_vsc_patterns_t m_patterns;

public:

    SW_VscFactory(const string &_app_name);
    virtual ~SW_VscFactory();

    int onLoad();
    AmSession *onInvite(const AmSipRequest &req, const string &app_name,
                        const map<string, string> &app_params);
    AmSession *onInvite(const AmSipRequest &req, const string &app_name,
                        AmArg &session_params);

};

class SW_VscDialog : public AmSession,
    public CredentialHolder
{
    AmAudioFile m_wav_file;

    sw_vsc_patterns_t *m_patterns;

    std::auto_ptr<UACAuthCred> cred;

    u_int64_t getAttributeId(MYSQL *my_handler, const char *attribute);
    u_int64_t getSubscriberId(MYSQL *my_handler, const char *uuid,
                              string *domain, u_int64_t &domain_id,
                              u_int64_t &profile_id, string *username);
    u_int64_t getPreference(MYSQL *my_handler, u_int64_t subscriberId, u_int64_t attributeId,
                            int *foundPref, string *value);
    int deletePreferenceId(MYSQL *my_handler, u_int64_t preferenceId);
    int insertPreference(MYSQL *my_handler, u_int64_t subscriberId,
                         u_int64_t attributeId, string &uri);
    int updatePreferenceId(MYSQL *my_handler, u_int64_t preferenceId, string &uri);
    int insertSpeedDialSlot(MYSQL *my_handler, u_int64_t subscriberId, string &slot, string &uri);
    int insertReminder(MYSQL *my_handler, u_int64_t subscriberId, string &repeat, string &tim);
    int deleteReminder(MYSQL *my_handler, u_int64_t subscriberId);
    int number2uri(const AmSipRequest &req, MYSQL *my_handler, string &uuid, u_int64_t subId,
                   string &domain, u_int64_t domId, int offset, string &uri,
                   string &username);
    int checkSubscriberProfile(MYSQL *my_handler, u_int64_t profileId, u_int64_t attributeId);
    u_int64_t createCFMap(MYSQL *my_handler, u_int64_t subscriberId, string &uri,
                          const char *mapName, const char *type);
    u_int64_t deleteCFMap(MYSQL *my_handler, u_int64_t subscriberId,
                          const char *mapName, const char *type);



public:
    SW_VscDialog(sw_vsc_patterns_t *patterns,
                 UACAuthCred *credentials = NULL);
    ~SW_VscDialog();

    void onSessionStart();
    void onStart();
    void onInvite(const AmSipRequest &req);
    void onBye(const AmSipRequest &req);
    void onDtmf(int event, int duration_msec) {}

    void process(AmEvent *event);

    UACAuthCred *getCredentials();
};

#endif
