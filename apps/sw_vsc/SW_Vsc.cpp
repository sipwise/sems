#include <re2/re2.h>
using namespace re2;

#include "SW_Vsc.h"
#include "AmConfig.h"
#include "AmUtils.h"
#include "AmPlugIn.h"

#include "sems.h"
#include "log.h"

//#include <my_global.h>
//#include <m_string.h>
#include <mysql.h>

#define MOD_NAME "sw_vsc"
#define SW_VSC_DATABASE "provisioning"

#define SW_VSC_GET_ATTRIBUTE_ID "select id from voip_preferences where attribute='%s'"
#define SW_VSC_GET_SUBSCRIBER_ID "select s.id, d.domain, d.id, s.profile_id, s.username "\
    "from voip_subscribers s, voip_domains d "\
    "where s.uuid='%s' and s.domain_id = d.id"
#define SW_VSC_GET_PREFERENCE_ID "select id,value from voip_usr_preferences where subscriber_id=%llu " \
    "and attribute_id=%llu"
#define SW_VSC_DELETE_PREFERENCE_ID "delete from voip_usr_preferences where id=%llu"
#define SW_VSC_INSERT_PREFERENCE "insert into voip_usr_preferences (subscriber_id, attribute_id, value) "\
    "values(%llu, %llu, '%s')"
#define SW_VSC_UPDATE_PREFERENCE_ID "update voip_usr_preferences set value='%s' where id=%llu"
#define SW_VSC_INSERT_SPEEDDIAL "replace into voip_speed_dial (subscriber_id, slot, destination) "\
    "values(%llu, '%s', '%s')"
#define SW_VSC_INSERT_REMINDER "replace into voip_reminder (subscriber_id, time, recur) "\
    "values(%llu, '%s', '%s')"
#define SW_VSC_DELETE_REMINDER "delete from voip_reminder where subscriber_id=%llu"
#define SW_VSC_GET_USER_CALLEE_REWRITE_DPID "select value from " \
    "voip_usr_preferences vup, voip_preferences vp where " \
    "vup.subscriber_id=%llu and vp.attribute='rewrite_callee_in_dpid' " \
    "and vup.attribute_id = vp.id"
#define SW_VSC_GET_USER_CALLEE_REWRITES \
    "select vrr.match_pattern, vrr.replace_pattern from " \
    "voip_rewrite_rules vrr, voip_rewrite_rule_sets vrrs " \
    "where vrrs.callee_in_dpid=%llu and vrr.set_id = vrrs.id " \
    "and vrr.direction='in' and vrr.field='callee' " \
    "and vrr.enabled = 1 order by vrr.priority asc"
#define SW_VSC_GET_DOMAIN_CALLEE_REWRITES \
    "select vrr.match_pattern, vrr.replace_pattern " \
    "from voip_rewrite_rules vrr, voip_rewrite_rule_sets vrrs, " \
    "voip_preferences vp, voip_dom_preferences vdp " \
    "where vdp.domain_id=%llu and vp.attribute='rewrite_callee_in_dpid' " \
    "and vdp.attribute_id = vp.id and  vdp.value = vrrs.callee_in_dpid " \
    "and vrr.set_id = vrrs.id and vrr.direction='in' and " \
    "vrr.field='callee' and vrr.enabled = 1 order by vrr.priority asc"

#define SW_VSC_GET_PRIMARY_NUMBER "select alias_username " \
    "from kamailio.dbaliases where username='%s' " \
    "and domain = '%s' and is_primary"

#define SW_VSC_DELETE_VSC_DESTSET "delete from voip_cf_destination_sets where "\
    "subscriber_id=%llu and name='%s'"
#define SW_VSC_CREATE_VSC_DESTSET "insert into voip_cf_destination_sets (subscriber_id, name) "\
    "values(%llu, '%s')"
#define SW_VSC_CREATE_VSC_DEST "insert into voip_cf_destinations (destination_set_id, destination, priority) "\
    "values(%llu, '%s', %d)"
#define SW_VSC_DELETE_VSC_CFMAP "delete from voip_cf_mappings where subscriber_id=%llu and type='%s'"
#define SW_VSC_CREATE_VSC_CFMAP "insert into voip_cf_mappings "\
    "(subscriber_id, type, destination_set_id, time_set_id) "\
    "values(%llu, '%s', %llu, NULL)"

#define SW_VSC_GET_SUBPROFILE_ATTRIBUTE "select id from voip_subscriber_profile_attributes "\
    "where profile_id=%llu and attribute_id=%llu"

#define SW_VSC_DESTSET_CFU  "cfu_by_vsc"
#define SW_VSC_DESTSET_CFB  "cfb_by_vsc"
#define SW_VSC_DESTSET_CFT  "cft_by_vsc"
#define SW_VSC_DESTSET_CFNA "cfna_by_vsc"
#define SW_VSC_DESTSET_CFS  "cfs_by_vsc"
#define SW_VSC_DESTSET_CFR  "cfr_by_vsc"
#define SW_VSC_DESTSET_CFO  "cfc_by_vsc"

#define CHECK_ANNOUNCEMENT_CONFIG(member, config_var) \
    m_patterns.member = cfg.getParameter(config_var, ""); \
    if (m_patterns.member.empty()) \
    { \
        ERROR(config_var " file not set\n"); \
    }

#define COMPILE_MATCH_PATTERN(member, config_var) \
    member = cfg.getParameter(config_var, ""); \
    if (member.empty()) \
    { \
        ERROR(config_var " is empty\n"); \
        member = "invalid_default_value"; \
    } \
    if (regcomp(&m_patterns.member, member.c_str(), REG_EXTENDED | REG_NOSUB)) \
    { \
        ERROR(config_var " failed to compile ('%s'): %s\n", \
              member.c_str(), \
              strerror(errno)); \
        return -1; \
    }

#define CHECK_ANNOUNCEMENT_PATH(member, config_var) \
    member = m_patterns->audioPath + lang + m_patterns->member; \
    if (m_patterns->member.empty() || !file_exists(member)) \
    { \
        ERROR(config_var " file does not exist ('%s').\n", \
              member.c_str()); \
        filename = failAnnouncement; \
        goto out; \
    }

EXPORT_SESSION_FACTORY(SW_VscFactory, MOD_NAME);

SW_VscFactory::SW_VscFactory(const string &_app_name)
    : AmSessionFactory(_app_name)
{
}

SW_VscFactory::~SW_VscFactory()
{
    regfree(&m_patterns.cfOffPattern);
    regfree(&m_patterns.cfuOnPattern);
    regfree(&m_patterns.cfuOffPattern);
    regfree(&m_patterns.cfbOnPattern);
    regfree(&m_patterns.cfbOffPattern);
    regfree(&m_patterns.cftOnPattern);
    regfree(&m_patterns.cftOffPattern);
    regfree(&m_patterns.cfnaOnPattern);
    regfree(&m_patterns.cfnaOffPattern);
    regfree(&m_patterns.speedDialPattern);
    regfree(&m_patterns.reminderOnPattern);
    regfree(&m_patterns.reminderOffPattern);
    regfree(&m_patterns.blockinclirOnPattern);
    regfree(&m_patterns.blockinclirOffPattern);
}

int SW_VscFactory::onLoad()
{
    string cfOffPattern;
    string cfuOnPattern;
    string cfuOffPattern;
    string cfbOnPattern;
    string cfbOffPattern;
    string cftOnPattern;
    string cftOffPattern;
    string cfnaOnPattern;
    string cfnaOffPattern;
    string speedDialPattern;
    string reminderOnPattern;
    string reminderOffPattern;
    string blockinclirOnPattern;
    string blockinclirOffPattern;

    AmConfigReader cfg;
    if (cfg.loadFile(AmConfig::ModConfigPath + string(MOD_NAME ".conf")))
        return -1;

    configureModule(cfg);


    m_patterns.mysqlHost = cfg.getParameter("mysql_host", "");
    if (m_patterns.mysqlHost.empty())
    {
        ERROR("MysqlHost is empty\n");
        return -1;
    }
    m_patterns.mysqlPort = cfg.getParameterInt("mysql_port", 0);
    if (m_patterns.mysqlPort < 0 || m_patterns.mysqlPort > 65535)
    {
        ERROR("MysqlPort is invalid\n");
        return -1;
    }
    m_patterns.mysqlUser = cfg.getParameter("mysql_user", "");
    if (m_patterns.mysqlUser.empty())
    {
        ERROR("MysqlUser is empty\n");
        return -1;
    }
    m_patterns.mysqlPass = cfg.getParameter("mysql_pass", "");
    if (m_patterns.mysqlPass.empty())
    {
        ERROR("MysqlPass is empty\n");
        return -1;
    }

    m_patterns.audioPath = cfg.getParameter("announce_path", ANNOUNCE_PATH);
    if (m_patterns.audioPath.empty())
    {
        ERROR("AnnouncePath option announce_path is empty.\n");
        return -1;
    }
    if (m_patterns.audioPath[m_patterns.audioPath.length() - 1] != '/' )
        m_patterns.audioPath += "/";

    CHECK_ANNOUNCEMENT_CONFIG(failAnnouncement,           "error_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(unknownAnnouncement,        "unknown_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfOffAnnouncement,          "cf_off_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfuOnAnnouncement,          "cfu_on_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfuOffAnnouncement,         "cfu_off_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfbOnAnnouncement,          "cfb_on_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfbOffAnnouncement,         "cfb_off_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cftOnAnnouncement,          "cft_on_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cftOffAnnouncement,         "cft_off_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfnaOnAnnouncement,         "cfna_on_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(cfnaOffAnnouncement,        "cfna_off_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(speedDialAnnouncement,      "speed_dial_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(reminderOnAnnouncement,     "reminder_on_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(reminderOffAnnouncement,    "reminder_off_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(blockinclirOnAnnouncement,  "blockinclir_on_announcement");
    CHECK_ANNOUNCEMENT_CONFIG(blockinclirOffAnnouncement, "blockinclir_off_announcement");


    // We could set a default in cfg.getParameter, but we really want to log the error
    // if the pattern in question is not set:

    m_patterns.voicemailNumber = cfg.getParameter("voicemail_number", "");
    if (m_patterns.voicemailNumber.empty())
    {
        ERROR("voicemail_number not set\n");
        m_patterns.voicemailNumber = "invalid_default_value";
    }

    COMPILE_MATCH_PATTERN(cfOffPattern,          "cf_off_pattern");
    COMPILE_MATCH_PATTERN(cfuOnPattern,          "cfu_on_pattern");
    COMPILE_MATCH_PATTERN(cfuOffPattern,         "cfu_off_pattern");
    COMPILE_MATCH_PATTERN(cfbOnPattern,          "cfb_on_pattern");
    COMPILE_MATCH_PATTERN(cfbOffPattern,         "cfb_off_pattern");
    COMPILE_MATCH_PATTERN(cftOnPattern,          "cft_on_pattern");
    COMPILE_MATCH_PATTERN(cftOffPattern,         "cft_off_pattern");
    COMPILE_MATCH_PATTERN(cfnaOnPattern,         "cfna_on_pattern");
    COMPILE_MATCH_PATTERN(cfnaOffPattern,        "cfna_off_pattern");
    COMPILE_MATCH_PATTERN(speedDialPattern,      "speed_dial_pattern");
    COMPILE_MATCH_PATTERN(reminderOnPattern,     "reminder_on_pattern");
    COMPILE_MATCH_PATTERN(reminderOffPattern,    "reminder_off_pattern");
    COMPILE_MATCH_PATTERN(blockinclirOnPattern,  "blockinclir_on_pattern");
    COMPILE_MATCH_PATTERN(blockinclirOffPattern, "blockinclir_off_pattern");

    return 0;
}

AmSession *SW_VscFactory::onInvite(const AmSipRequest &req,
                                   const string &app_name,
                                   const map<string, string> &app_params)
{
    return new SW_VscDialog(&m_patterns, NULL);
}

AmSession *SW_VscFactory::onInvite(const AmSipRequest &req,
                                   const string &app_name,
                                   AmArg &session_params)
{
    UACAuthCred *cred = AmUACAuth::unpackCredentials(session_params);
    AmSession *s = new SW_VscDialog(&m_patterns, cred);

    if (NULL == cred)
    {
        WARN("discarding unknown session parameters.\n");
    }
    else
    {
        AmUACAuth::enable(s);
    }

    return s;
}

SW_VscDialog::SW_VscDialog(sw_vsc_patterns_t *patterns,
                           UACAuthCred *credentials)
    : m_patterns(patterns), cred(credentials)
{
}

SW_VscDialog::~SW_VscDialog()
{
}

void SW_VscDialog::onSessionStart()
{
    DBG("SW_VscDialog::onSessionStart()...\n");
    AmSession::onSessionStart();
}

void SW_VscDialog::onStart() {
}

u_int64_t SW_VscDialog::getAttributeId(MYSQL *my_handler, const char *attribute)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    char query[1024] = "";
    u_int64_t id;

    snprintf(query, sizeof(query), SW_VSC_GET_ATTRIBUTE_ID, attribute);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error fetching id for attribute '%s': %s", attribute,
              mysql_error(my_handler));
        return 0;
    }

    res = mysql_store_result(my_handler);
    if (mysql_num_rows(res) != 1)
    {
        ERROR("Found invalid number of id entries for attribute '%s': %lu",
              attribute, mysql_num_rows(res));
        return 0;
    }

    row = mysql_fetch_row(res);
    if (row == NULL || row[0] == NULL)
    {
        ERROR("Failed to fetch row for attribute id: %s\n",
              mysql_error(my_handler));
        return 0;
    }

    id = atoll(row[0]);
    mysql_free_result(res);
    return id;
}

int SW_VscDialog::checkSubscriberProfile(MYSQL *my_handler, u_int64_t profileId, u_int64_t attributeId)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    char query[1024] = "";
    int ret = 0;

    // if no profile is set, allow by default
    if(profileId == 0)
    {
        INFO("Allow preference due to unset subscriber profile");
        return 1;
    }

    snprintf(query, sizeof(query), SW_VSC_GET_SUBPROFILE_ATTRIBUTE,
             (unsigned long long int) profileId, (unsigned long long int) attributeId);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error checking profile attributes for profile '%llu' and attribute %llu: %s",
              (unsigned long long int) profileId, (unsigned long long int) attributeId,
              mysql_error(my_handler));
        return 0;
    }

    res = mysql_store_result(my_handler);
    if (mysql_num_rows(res) >= 1)
    {
        INFO("Allow preference attribute %llu as it is in profile %llu",
             (unsigned long long int) attributeId, (unsigned long long int) profileId);
        ret = 1;
    }
    else
    {
        INFO("Reject preference attribute %llu as it is not in profile %llu",
             (unsigned long long int) attributeId, (unsigned long long int) profileId);
        ret = 0;
    }
    return ret;
}

u_int64_t SW_VscDialog::getSubscriberId(MYSQL *my_handler, const char *uuid,
                                        string *domain, u_int64_t &domain_id,
                                        u_int64_t &profile_id, string *username)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    char query[1024] = "";
    u_int64_t id;

    snprintf(query, sizeof(query), SW_VSC_GET_SUBSCRIBER_ID, uuid);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error fetching id for subscriber '%s': %s", uuid,
              mysql_error(my_handler));
        return 0;
    }

    res = mysql_store_result(my_handler);
    if (mysql_num_rows(res) != 1)
    {
        ERROR("Found invalid number of id entries for uuid '%s': %lu",
              uuid , mysql_num_rows(res));
        return 0;
    }

    row = mysql_fetch_row(res); if (row == NULL || row[0] == NULL || row[1] ==
                                    NULL || row[2] == NULL)
    {
        ERROR("Failed to fetch row for uuid id: %s\n",
              mysql_error(my_handler)); return 0;
    }

    id = atoll(row[0]);
    domain->clear();
    *domain += string(row[1]);
    domain_id = atoll(row[2]);
    profile_id = row[3] == NULL ? 0 : atoll(row[3]);
    username->clear();
    *username += string(row[4]);
    mysql_free_result(res);
    return id;
}

u_int64_t SW_VscDialog::getPreference(MYSQL *my_handler, u_int64_t subscriberId,
                                      u_int64_t attributeId,
                                      int *foundPref, string *value)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    char query[1024] = "";
    u_int64_t id;
    *foundPref = 0;

    snprintf(query, sizeof(query), SW_VSC_GET_PREFERENCE_ID,
             (unsigned long long int)subscriberId,
             (unsigned long long int)attributeId);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error fetching preference id for subscriber id '%llu' and attribute id '%llu': %s",
              (unsigned long long int)subscriberId,
              (unsigned long long int)attributeId, mysql_error(my_handler));
        return 0;
    }

    res = mysql_store_result(my_handler);
    if (mysql_num_rows(res) == 0)
    {
        mysql_free_result(res);
        return 1;
    }
    if (mysql_num_rows(res) != 1)
    {
        ERROR("Found invalid number of id entries for subscriber id '%llu' and attribute id '%llu': %lu",
              (unsigned long long int)subscriberId,
              (unsigned long long int)attributeId, mysql_num_rows(res));
        mysql_free_result(res);
        return 0;
    }

    row = mysql_fetch_row(res);
    if (row == NULL || row[0] == NULL)
    {
        ERROR("Failed to fetch row for preference id: %s\n",
              mysql_error(my_handler));
        mysql_free_result(res);
        return 0;
    }
    id = atoll(row[0]);
    value->clear();
    *value += row[1];
    mysql_free_result(res);
    *foundPref = 1;
    return id;
}

int SW_VscDialog::deletePreferenceId(MYSQL *my_handler, u_int64_t preferenceId)
{
    char query[1024] = "";

    snprintf(query, sizeof(query), SW_VSC_DELETE_PREFERENCE_ID,
             (unsigned long long int)preferenceId);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error deleting preference id '%llu': %s",
              (unsigned long long int)preferenceId, mysql_error(my_handler));
        return 0;
    }
    return 1;
}

int SW_VscDialog::insertPreference(MYSQL *my_handler, u_int64_t subscriberId,
                                   u_int64_t attributeId, string &uri)
{
    char query[1024] = "";

    snprintf(query, sizeof(query), SW_VSC_INSERT_PREFERENCE,
             (unsigned long long int)subscriberId,
             (unsigned long long int)attributeId, uri.c_str());

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error inserting preference for subscriber id '%llu': %s",
              (unsigned long long int)subscriberId, mysql_error(my_handler));
        return 0;
    }
    return 1;
}

int SW_VscDialog::updatePreferenceId(MYSQL *my_handler,
                                     u_int64_t preferenceId, string &uri)
{
    char query[1024] = "";

    snprintf(query, sizeof(query), SW_VSC_UPDATE_PREFERENCE_ID,
             uri.c_str(), (unsigned long long int)preferenceId);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error updating preference id '%llu': %s",
              (unsigned long long int)preferenceId, mysql_error(my_handler));
        return 0;
    }
    return 1;
}

int SW_VscDialog::insertReminder(MYSQL *my_handler, u_int64_t subscriberId,
                                 string &repeat, string &tim)
{
    char query[1024] = "";

    snprintf(query, sizeof(query), SW_VSC_INSERT_REMINDER,
             (unsigned long long int)subscriberId,
             tim.c_str(), repeat.c_str());

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error setting reminder for subscriber id '%llu': %s",
              (unsigned long long int)subscriberId, mysql_error(my_handler));
        return 0;
    }
    return 1;
}

int SW_VscDialog::deleteReminder(MYSQL *my_handler, u_int64_t subscriberId)
{
    char query[1024] = "";

    snprintf(query, sizeof(query), SW_VSC_DELETE_REMINDER,
             (unsigned long long int)subscriberId);

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error deleting reminder for subscriber id '%llu': %s",
              (unsigned long long int)subscriberId, mysql_error(my_handler));
        return 0;
    }
    return 1;
}

int SW_VscDialog::insertSpeedDialSlot(MYSQL *my_handler,
                                      u_int64_t subscriberId,
                                      string &slot, string &uri)
{
    char query[1024] = "";

    snprintf(query, sizeof(query), SW_VSC_INSERT_SPEEDDIAL,
             (unsigned long long int)subscriberId, slot.c_str(), uri.c_str());

    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error inserting speed-dial slot '%s' for subscriber id '%llu': %s",
              slot.c_str(), (unsigned long long int)subscriberId,
              mysql_error(my_handler));
        return 0;
    }
    return 1;
}


int SW_VscDialog::number2uri(const AmSipRequest &req, MYSQL *my_handler,
                             string &uuid, u_int64_t subId,
                             string &domain, u_int64_t domId,
                             int offset, string &uri, string &username)
{
    string num = "", acStr = "", ccStr = "";
    u_int64_t acAttId, ccAttId;
    int foundPref;
    char query[1024] = "";
    my_ulonglong num_rows;
    MYSQL_RES *res;
    MYSQL_ROW row;

    if (req.user.compare(0, 1, "*", 1) == 0)
    {
        num = req.user.substr(offset);

        if (m_patterns->voicemailNumber == num)
        {
            snprintf(query, sizeof(query),
                     SW_VSC_GET_PRIMARY_NUMBER,
                     username.c_str(), domain.c_str());
            if (mysql_real_query(my_handler, query, strlen(query)) != 0)
            {
                ERROR("Error fetching primary number for username '%s'"
                    " at domain '%s': %s",
                      username.c_str(), domain.c_str(),
                      mysql_error(my_handler));
                return 0;
            }
            res = mysql_store_result(my_handler);
            row = mysql_fetch_row(res);
            if (row == NULL || row[0] == NULL)
            {
                INFO("no primary number for username '%s'"
                    " at domain '%s' found. Use uuid: %s",
                      username.c_str(), domain.c_str(),
                      uuid.c_str());
                uri = string("sip:vmu") + uuid + "@voicebox.local";
            }
            else {
                uri = string("sip:vmu") + row[0] + "@voicebox.local";
            }
            INFO("Normalized '%s' to voicemail uri '%s' for uuid '%s'",
                num.c_str(), uri.c_str(), uuid.c_str());
            mysql_free_result(res);
            return 1;
        }

        snprintf(query, sizeof(query),
                 SW_VSC_GET_USER_CALLEE_REWRITE_DPID,
                 (unsigned long long int)subId);
        if (mysql_real_query(my_handler, query, strlen(query)) != 0)
        {
            ERROR("Error fetching rewrite rule dpid for subscriber uuid '%s' (id %llu): %s",
                  uuid.c_str(), (unsigned long long int)subId,
                  mysql_error(my_handler));
            return 0;
        }
        res = mysql_store_result(my_handler);
        if ((num_rows = mysql_num_rows(res)) != 0)
        {
            row = mysql_fetch_row(res);
            if (row == NULL || row[0] == NULL)
            {
                ERROR("Failed to fetch row for user callee rewrite rule: %s\n", mysql_error(my_handler));
                mysql_free_result(res);
                return 0;
            }
            u_int64_t dpid = atoll(row[0]);
            INFO("Found user rewrite rule dpid '%llu' for subscriber uuid '%s' (id %llu)",
                 (unsigned long long int)dpid, uuid.c_str(),
                 (unsigned long long int)subId);
            mysql_free_result(res);
            snprintf(query, sizeof(query), SW_VSC_GET_USER_CALLEE_REWRITES,
                     (unsigned long long int)dpid);
            if (mysql_real_query(my_handler, query, strlen(query)) != 0)
            {
                ERROR("Error fetching user callee rewrite rules for dpid '%llu' for subscriber uuid '%s': %s",
                      (unsigned long long int)dpid, uuid.c_str(),
                      mysql_error(my_handler));
                return 0;
            }
            res = mysql_store_result(my_handler);
            if ((num_rows = mysql_num_rows(res)) == 0)
            {
                mysql_free_result(res);
                uri = string("sip:+") + num + "@" + domain;
                INFO("No user callee rewrite rules for subscriber uuid '%s' (id '%llu')",
                     uuid.c_str(), (unsigned long long int)subId);
                return 1;
            }
        }
        else
        {
            mysql_free_result(res);
            INFO("No user rewrite rule for subscriber uuid '%s' (id %llu), falling back to domain '%s' (id %llu)",
                 uuid.c_str(), (unsigned long long int)subId,
                 domain.c_str(), (unsigned long long int)domId);

            snprintf(query, sizeof(query), SW_VSC_GET_DOMAIN_CALLEE_REWRITES,
                     (unsigned long long int)domId);
            if (mysql_real_query(my_handler, query, strlen(query)) != 0)
            {
                ERROR("Error fetching domain callee rewrite rules for domain id '%llu' (domain '%s') for subscriber uuid '%s': %s",
                      (unsigned long long int)domId, domain.c_str(),
                      uuid.c_str(), mysql_error(my_handler));
                return 0;
            }
            res = mysql_store_result(my_handler);
            if ((num_rows = mysql_num_rows(res)) == 0)
            {
                mysql_free_result(res);
                uri = string("sip:+") + num + "@" + domain;
                INFO("No domain callee rewrite rules for domain id '%llu' (domain '%s') for subscriber uuid '%s'",
                     (unsigned long long int)domId, domain.c_str(),
                     uuid.c_str());
                return 1;
            }
        }

        for (my_ulonglong i = 0; i < num_rows; ++i)
        {
            row = mysql_fetch_row(res);
            if (row == NULL || row[0] == NULL || row[1] == NULL)
            {
                ERROR("Failed to fetch row for callee rewrite rule: %s\n",
                      mysql_error(my_handler));
                mysql_free_result(res);
                return 0;
            }

            RE2 re(row[0]);
            if (re.error().length() > 0)
            {
                ERROR("A callee rewrite rule match pattern ('%s') failed to compile: %s\n",
                      row[0], re.error().c_str());
                mysql_free_result(res);
                return 0;
            }
            if (RE2::Replace(&num, re, row[1]))
            {
                INFO("The callee rewrite rule pattern ('%s' and '%s') for subscriber id %llu and domain id %llu matched, result is '%s'\n",
                     row[0], row[1], (unsigned long long int)subId,
                     (unsigned long long int)domId, num.c_str());
                break;
            }
        }

        mysql_free_result(res);

        // we need to replace $avp(s:caller_ac) and $avp(s:caller_cc)
        // with the actual values here

        acAttId = getAttributeId(my_handler, "ac");
        if (!acAttId)
            return 0;
        ccAttId = getAttributeId(my_handler, "cc");
        if (!ccAttId)
            return 0;
        if (!getPreference(my_handler, subId, acAttId, &foundPref, &acStr))
            return 0;
        if (!getPreference(my_handler, subId, ccAttId, &foundPref, &ccStr))
            return 0;

        if (RE2::GlobalReplace(&num, "\\$avp\\(s:caller_ac\\)", acStr))
        {
            INFO("Successfully replaced $avp(s:caller_ac) by preference value '%s' for subscriber id %llu and domain id %llu",
                 acStr.c_str(), (unsigned long long int)subId,
                 (unsigned long long int)domId);
        }
        if (RE2::GlobalReplace(&num, "\\$avp\\(s:caller_cc\\)", ccStr))
        {
            INFO("Successfully replaced $avp(s:caller_cc) by preference value '%s' for subscriber id %llu and domain id %llu",
                 ccStr.c_str(), (unsigned long long int)subId,
                 (unsigned long long int)domId);
        }

        INFO("Final normalized number is '%s' for subscriber id %llu and domain id %llu",
             num.c_str(), (unsigned long long int)subId,
             (unsigned long long int)domId);
        uri = string("sip:+") + num + "@" + domain;
    }
    return 1;
}

u_int64_t SW_VscDialog::createCFMap(MYSQL *my_handler, u_int64_t subscriberId,
                                    string &uri, const char *mapName,
                                    const char *type)
{
    u_int64_t dsetId;
    u_int64_t mapId;
    char query[1024] = "";

    // first, delete potentially existing vsc destination set for subscriber
    snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_DESTSET,
             (unsigned long long int)subscriberId, mapName);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error deleting existing CF destination set '%s' for subscriber id '%llu': %s",
              mapName, (unsigned long long int)subscriberId,
              mysql_error(my_handler));
        return 0;
    }

    // create a new destination set
    snprintf(query, sizeof(query), SW_VSC_CREATE_VSC_DESTSET,
             (unsigned long long int)subscriberId, mapName);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error creating new CF destination set '%s' for subscriber id '%llu': %s",
              mapName, (unsigned long long int)subscriberId,
              mysql_error(my_handler));
        return 0;
    }
    dsetId = mysql_insert_id(my_handler);
    if (!dsetId)
    {
        ERROR("Error fetching last insert id of CF destination set for subscriber id '%llu'",
              (unsigned long long int)subscriberId);
        return 0;
    }

    // create new destination entry
    snprintf(query, sizeof(query), SW_VSC_CREATE_VSC_DEST,
             (unsigned long long int)dsetId,
             uri.c_str(), 1);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error creating new CF destination '%s' for destination set id '%llu': %s",
              uri.c_str(), (unsigned long long int)dsetId,
              mysql_error(my_handler));
        return 0;
    }

    // delete existing mapping
    snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_CFMAP,
             (unsigned long long int)subscriberId, type);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error deleting existing CF destination mapping for subscriber id '%llu' and type '%s': %s",
              (unsigned long long int)subscriberId, type,
              mysql_error(my_handler));
        return 0;
    }

    // create new mapping
    snprintf(query, sizeof(query), SW_VSC_CREATE_VSC_CFMAP,
             (unsigned long long int)subscriberId,
             type, (unsigned long long int)dsetId);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error creating CF destination mapping for subscriber id '%llu' and type '%s' to destination set id '%llu': %s",
              (unsigned long long int)subscriberId, type,
              (unsigned long long int)dsetId, mysql_error(my_handler));
        return 0;
    }
    mapId = mysql_insert_id(my_handler);
    if (!mapId)
    {
        ERROR("Error fetching last insert id of CF mapping for subscriber id '%llu' and type '%s'",
              (unsigned long long int)subscriberId, type);
        return 0;
    }

    return mapId;

}

u_int64_t SW_VscDialog::deleteCFMap(MYSQL *my_handler, u_int64_t subscriberId,
                                    const char *mapName, const char *type)
{
    char query[1024] = "";

    // first, delete potentially existing vsc destination set for subscriber
    snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_DESTSET,
             (unsigned long long int)subscriberId, mapName);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error deleting existing CF destination set '%s' for subscriber id '%llu': %s",
              mapName, (unsigned long long int)subscriberId,
              mysql_error(my_handler));
        return 0;
    }

    // delete existing mapping
    snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_CFMAP,
             (unsigned long long int)subscriberId, type);
    if (mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
        ERROR("Error deleting existing CF destination mapping for subscriber id '%llu' and type '%s': %s",
              (unsigned long long int)subscriberId, type,
              mysql_error(my_handler));
        return 0;
    }

    return 1;
}

u_int64_t SW_VscDialog::deleteCF(MYSQL *my_handler, u_int64_t subscriberId,
                                 const char *mapName, const char *type,
                                 int *foundPref, string *value, const char *uuid)
{
    if (!deleteCFMap(my_handler, subscriberId, mapName, type))
    {
        return 0;
    }

    u_int64_t attId = getAttributeId(my_handler, type);
    if (!attId)
    {
        return 0;
    }
    
    u_int64_t prefId = getPreference(my_handler, subscriberId, attId, foundPref, value);
    if (!prefId)
    {
        return 0;
    }
    else if (!*foundPref)
    {
        INFO("Unnecessary VSC %s removal for uuid '%s'",
             type, uuid);
    }
    else if (!deletePreferenceId(my_handler, prefId))
    {
        return 0;
    }
    else
    {
        INFO("Successfully removed VSC %s for uuid '%s'",
             type, uuid);
    }

    return 1;
}

void SW_VscDialog::onInvite(const AmSipRequest &req)
{
    /// fooooo
    if(dlg->getStatus() == AmSipDialog::Connected){
      AmSession::onInvite(req);
      return;
    }

    int ret;
    string filename;
    MYSQL *my_handler = NULL;
    my_bool recon = 1;

    u_int64_t subId, domId, profId;
    string domain, uri, username;
    char map_str[128] = "";
    string mapStr, prefStr;
    int foundPref = 0;

    string failAnnouncement;
    string unknownAnnouncement;
    string cfOffAnnouncement;
    string cfuOnAnnouncement;
    string cfuOffAnnouncement;
    string cfbOnAnnouncement;
    string cfbOffAnnouncement;
    string cftOnAnnouncement;
    string cftOffAnnouncement;
    string cfnaOnAnnouncement;
    string cfnaOffAnnouncement;
    string speedDialAnnouncement;
    string reminderOnAnnouncement;
    string reminderOffAnnouncement;
    string blockinclirOnAnnouncement;
    string blockinclirOffAnnouncement;

    string uuid = getHeader(req.hdrs, "P-Caller-UUID");
    if (!uuid.length())
    {
        ERROR("Application header P-Caller-UUID not found\n");
        throw AmSession::Exception(500, "could not get UUID parameter");
    }
    string lang = getHeader(req.hdrs, "P-Language");
    if (!lang.length())
    {
        lang = "en";
    }
    lang += "/";


    failAnnouncement = m_patterns->audioPath + lang + m_patterns->failAnnouncement;
    if (m_patterns->failAnnouncement.empty() || !file_exists(failAnnouncement))
    {
        ERROR("ErrorAnnouncement file does not exist ('%s').\n",
              failAnnouncement.c_str());
        throw AmSession::Exception(500, "could not get failed announcement");
    }
    unknownAnnouncement = m_patterns->audioPath + lang + m_patterns->unknownAnnouncement;
    if (m_patterns->unknownAnnouncement.empty() || !file_exists(unknownAnnouncement))
    {
        ERROR("UnknownAnnouncement file does not exist ('%s').\n",
              unknownAnnouncement.c_str());
        filename = failAnnouncement;
        goto out;
    }


    my_handler = mysql_init(NULL);
    if (!mysql_real_connect(my_handler,
                            m_patterns->mysqlHost.c_str(),
                            m_patterns->mysqlUser.c_str(),
                            m_patterns->mysqlPass.c_str(),
                            SW_VSC_DATABASE, m_patterns->mysqlPort, NULL, 0))
    {
        ERROR("Error connecting to provisioning db: %s",
              mysql_error(my_handler));
        filename = failAnnouncement;
        goto out;
    }
    if (mysql_options(my_handler, MYSQL_OPT_RECONNECT, &recon) != 0)
    {
        ERROR("Error setting reconnect-option for provisioning db: %s",
              mysql_error(my_handler));
        filename = failAnnouncement;
        goto out;
    }

    subId = getSubscriberId(my_handler, uuid.c_str(),
        &domain, domId, profId, &username);
    if (!subId)
    {
        filename = failAnnouncement;
        goto out;
    }

    DBG("Trigger VSC for uuid '%s' (sid='%llu')\n",
        uuid.c_str(), (unsigned long long int)subId);

    setReceiving(false);

    if ((ret = regexec(&m_patterns->cfOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        u_int64_t attId, prefId;

        CHECK_ANNOUNCEMENT_PATH(cfOffAnnouncement, "cf_off_announcement");

        /// Remove CFU
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFU, "cfu", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        /// Remove CFB
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFB, "cfb", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        /// Remove CFT
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFT, "cft", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        attId = getAttributeId(my_handler, "ringtimeout");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }

        prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (foundPref && !deletePreferenceId(my_handler, prefId))
        {
            filename = failAnnouncement;
            goto out;
        }
        else
        {
            INFO("Successfully removed VSC cft ringtimeout for uuid '%s'",
                 uuid.c_str());
        }

        /// Remove CFNA
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFNA, "cfna", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        /// Remove CFS
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFS, "cfs", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        /// Remove CFR
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFR, "cfr", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        /// Remove CFO
        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFO, "cfo", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        /// END
        filename = cfOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cfuOnPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
		CHECK_ANNOUNCEMENT_PATH(cfuOnAnnouncement, "cfu_on_announcement");

        u_int64_t attId = getAttributeId(my_handler, "cfu");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }

        if(!checkSubscriberProfile(my_handler, profId, attId))
        {
            filename = failAnnouncement;
            goto out;
        }

        if (!number2uri(req, my_handler, uuid, subId, domain, domId, 4,
                uri, username))
        {
            filename = failAnnouncement;
            goto out;
        }

        u_int64_t mapId = createCFMap(my_handler, subId, uri,
                                      SW_VSC_DESTSET_CFU, "cfu");
        if (!mapId)
        {
            filename = failAnnouncement;
            goto out;
        }
        snprintf(map_str, sizeof(mapStr), "%llu",
                 (unsigned long long int)mapId);
        mapStr = string(map_str);

        u_int64_t prefId = getPreference(my_handler, subId, attId,
                                         &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            if (!insertPreference(my_handler, subId, attId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully set VSC CFU to '%s' using mapping id '%llu' for uuid '%s'",
                 uri.c_str(), (unsigned long long int)mapId, uuid.c_str());
        }
        else
        {
            if (!updatePreferenceId(my_handler, prefId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully updated VSC CFU to '%s' using mapping id '%llu' for uuid '%s'",
                 uri.c_str(), (unsigned long long int)mapId, uuid.c_str());
        }

        filename = cfuOnAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cfuOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(cfuOffAnnouncement, "cfu_off_announcement");

        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFU, "cfu", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        filename = cfuOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cfbOnPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
		CHECK_ANNOUNCEMENT_PATH(cfbOnAnnouncement, "cfb_on_announcement");

        u_int64_t attId = getAttributeId(my_handler, "cfb");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }

        if(!checkSubscriberProfile(my_handler, profId, attId))
        {
            filename = failAnnouncement;
            goto out;
        }

        if (!number2uri(req, my_handler, uuid, subId, domain, domId, 4,
                uri, username))
        {
            filename = failAnnouncement;
            goto out;
        }

        u_int64_t mapId = createCFMap(my_handler, subId, uri,
                                      SW_VSC_DESTSET_CFB, "cfb");
        if (!mapId)
        {
            filename = failAnnouncement;
            goto out;
        }
        snprintf(map_str, sizeof(mapStr), "%llu",
                 (unsigned long long int)mapId);
        mapStr = string(map_str);

        u_int64_t prefId = getPreference(my_handler, subId, attId,
                                         &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            if (!insertPreference(my_handler, subId, attId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully set VSC CFB to '%s' for uuid '%s'",
                 uri.c_str(), uuid.c_str());
        }
        else
        {
            if (!updatePreferenceId(my_handler, prefId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully updated VSC CFB to '%s' for uuid '%s'",
                 uri.c_str(), uuid.c_str());
        }

        filename = cfbOnAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cfbOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(cfbOffAnnouncement, "cfb_off_announcement");

        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFB, "cfb", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        filename = cfbOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cftOnPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(cftOnAnnouncement, "cft_on_announcement");

        u_int64_t attId = getAttributeId(my_handler, "cft");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }

        if(!checkSubscriberProfile(my_handler, profId, attId))
        {
            filename = failAnnouncement;
            goto out;
        }

        string::size_type timend = req.user.find('*', 4);
        string tim = req.user.substr(4, timend - 4);
        INFO("Extracted ringtimeout of '%s' from '%s' for uuid '%s'",
             tim.c_str(), req.user.c_str(), uuid.c_str());

        if (!number2uri(req, my_handler, uuid, subId, domain,
                        domId, timend + 1, uri, username))
        {
            filename = failAnnouncement;
            goto out;
        }

        u_int64_t mapId = createCFMap(my_handler, subId, uri,
                                      SW_VSC_DESTSET_CFT, "cft");
        if (!mapId)
        {
            filename = failAnnouncement;
            goto out;
        }
        snprintf(map_str, sizeof(mapStr), "%llu",
                 (unsigned long long int)mapId);
        mapStr = string(map_str);

        u_int64_t prefId = getPreference(my_handler, subId, attId,
                                         &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            if (!insertPreference(my_handler, subId, attId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully set VSC CFT to '%s' for uuid '%s'",
                 uri.c_str(), uuid.c_str());
        }
        else
        {
            if (!updatePreferenceId(my_handler, prefId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully updated VSC CFT to '%s' for uuid '%s'",
                 uri.c_str(), uuid.c_str());
        }

        attId = getAttributeId(my_handler, "ringtimeout");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }
        prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            if (!insertPreference(my_handler, subId, attId, tim))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully set VSC ringtimeout to '%s' for uuid '%s'",
                 tim.c_str(), uuid.c_str());
        }
        else
        {
            if (!updatePreferenceId(my_handler, prefId, tim))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully updated VSC ringtimeout to '%s' for uuid '%s'",
                 tim.c_str(), uuid.c_str());
        }

        filename = cftOnAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cftOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(cftOffAnnouncement, "cft_off_announcement");

        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFT, "cft", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        u_int64_t attId = getAttributeId(my_handler, "ringtimeout");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }
        u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (foundPref && !deletePreferenceId(my_handler, prefId))
        {
            filename = failAnnouncement;
            goto out;
        }
        else
        {
            INFO("Successfully removed VSC CFT ringtimeout for uuid '%s'",
                 uuid.c_str());
        }


        filename = cftOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cfnaOnPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(cfnaOnAnnouncement, "cfna_on_announcement");

        u_int64_t attId = getAttributeId(my_handler, "cfna");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }

        if(!checkSubscriberProfile(my_handler, profId, attId))
        {
            filename = failAnnouncement;
            goto out;
        }

        if (!number2uri(req, my_handler, uuid, subId, domain, domId, 4,
                uri, username))
        {
            filename = failAnnouncement;
            goto out;
        }

        u_int64_t mapId = createCFMap(my_handler, subId, uri,
                                      SW_VSC_DESTSET_CFNA, "cfna");
        if (!mapId)
        {
            filename = failAnnouncement;
            goto out;
        }
        snprintf(map_str, sizeof(mapStr), "%llu",
                 (unsigned long long int)mapId);
        mapStr = string(map_str);

        u_int64_t prefId = getPreference(my_handler, subId, attId,
                                         &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            if (!insertPreference(my_handler, subId, attId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully set VSC CFNA to '%s' for uuid '%s'",
                 uri.c_str(), uuid.c_str());
        }
        else
        {
            if (!updatePreferenceId(my_handler, prefId, mapStr))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully updated VSC CFNA to '%s' for uuid '%s'",
                 uri.c_str(), uuid.c_str());
        }

        filename = cfnaOnAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->cfnaOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(cfnaOffAnnouncement, "cfna_off_announcement");

        if(!deleteCF(my_handler, subId, SW_VSC_DESTSET_CFNA, "cfna", &foundPref, &prefStr, uuid.c_str()))
        {
            filename = failAnnouncement;
            goto out;
        }

        filename = cfnaOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->speedDialPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(speedDialAnnouncement, "speed_dial_announcement");

        string slot = string("*") + req.user.substr(4, 1);
        if (!number2uri(req, my_handler, uuid, subId, domain, domId, 5,
                uri, username))
        {
            filename = failAnnouncement;
            goto out;
        }
        if (!insertSpeedDialSlot(my_handler, subId, slot, uri))
        {
            filename = failAnnouncement;
            goto out;
        }
        else
        {
            INFO("Successfully set speed-dial slot '%s' for uuid '%s'",
                 slot.c_str(), uuid.c_str());
        }

        filename = speedDialAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->reminderOnPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(reminderOnAnnouncement, "reminder_on_announcement");

        int hour, min;
        string tim; char c_tim[6] = "";
        hour = atoi(req.user.substr(4, 2).c_str());
        min = atoi(req.user.substr(6, 2).c_str());

        if (hour < 0 || hour > 23)
        {
            filename = failAnnouncement;
            INFO("Invalid hour '%s' in reminder data for uuid '%s'",
                 req.user.substr(4, 2).c_str(), uuid.c_str());
            goto out;
        }
        if (min < 0 || min > 59)
        {
            filename = failAnnouncement;
            INFO("Invalid minute '%s' in reminder data for uuid '%s'",
                 req.user.substr(6, 2).c_str(), uuid.c_str());
            goto out;
        }
        snprintf(c_tim, sizeof(c_tim), "%02d:%02d", hour, min);
        tim = string(c_tim);
        string recur("never");

        if (!insertReminder(my_handler, subId, recur, tim))
        {
            filename = failAnnouncement;
            goto out;
        }
        else
        {
            INFO("Successfully set reminder at '%s' for uuid '%s'",
                 c_tim, uuid.c_str());
        }

        filename = reminderOnAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->reminderOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(reminderOffAnnouncement, "reminder_off_announcement");

        if (!deleteReminder(my_handler, subId))
        {
            filename = failAnnouncement;
            goto out;
        }
        else
        {
            INFO("Successfully deleted reminder for uuid '%s'",
                 uuid.c_str());
        }

        filename = reminderOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }


    if ((ret = regexec(&m_patterns->blockinclirOnPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(blockinclirOnAnnouncement, "blockinclir_on_announcement");

        std::string val = "1";
        u_int64_t attId = getAttributeId(my_handler, "block_in_clir");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }
        u_int64_t prefId = getPreference(my_handler, subId, attId,
                                         &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            if (!insertPreference(my_handler, subId, attId, val))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully set VSC block_in_clir for uuid '%s'",
                 uuid.c_str());
        }
        else
        {
            if (!updatePreferenceId(my_handler, prefId, val))
            {
                filename = failAnnouncement;
                goto out;
            }
            INFO("Successfully updated VSC block_in_clir for uuid '%s'",
                 uuid.c_str());
        }

        filename = blockinclirOnAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }

    if ((ret = regexec(&m_patterns->blockinclirOffPattern,
                       req.user.c_str(), 0, 0, 0)) == 0)
    {
        CHECK_ANNOUNCEMENT_PATH(blockinclirOffAnnouncement, "blockinclir_off_announcement");

        u_int64_t attId = getAttributeId(my_handler, "block_in_clir");
        if (!attId)
        {
            filename = failAnnouncement;
            goto out;
        }
        u_int64_t prefId = getPreference(my_handler, subId, attId,
                                         &foundPref, &prefStr);
        if (!prefId)
        {
            filename = failAnnouncement;
            goto out;
        }
        else if (!foundPref)
        {
            INFO("Unnecessary VSC block_in_clir removal for uuid '%s'",
                 uuid.c_str());
        }
        else if (!deletePreferenceId(my_handler, prefId))
        {
            filename = failAnnouncement;
            goto out;
        }
        else
        {
            INFO("Successfully removed block_in_clir for uuid '%s'",
                 uuid.c_str());
        }

        filename = blockinclirOffAnnouncement;
        goto out;
    }
    else if (ret != REG_NOMATCH)
    {
        filename = failAnnouncement;
        goto out;
    }


    INFO("Unkown VSC code '%s' found", req.user.c_str());
    filename = unknownAnnouncement;

out:

    if (my_handler != NULL)
    {
        mysql_close(my_handler);
    }
    if (m_wav_file.open(filename, AmAudioFile::Read))
        throw string("SW_VscDialog::onSessionStart: Cannot open file\n");


    setOutput(&m_wav_file);

    AmSession::onInvite(req);
}

void SW_VscDialog::onBye(const AmSipRequest &req)
{
    DBG("onBye: stopSession\n");
    AmSession::onBye(req);
}


void SW_VscDialog::process(AmEvent *event)
{

    AmAudioEvent *audio_event = dynamic_cast<AmAudioEvent *>(event);
    if (audio_event && (audio_event->event_id == AmAudioEvent::cleared))
    {
        dlg->bye();
        setStopped();
        return;
    }

    AmSession::process(event);
}

inline UACAuthCred *SW_VscDialog::getCredentials()
{
    return cred.get();
}
