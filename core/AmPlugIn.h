/*
 * Copyright (C) 2002-2003 Fhg Fokus
 * Copyright (C) 2006 iptego GmbH
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
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
/** @file AmPlugIn.h */
#ifndef _AmPlugIn_h_
#define _AmPlugIn_h_

#include "AmThread.h"

#include <string>
#include <map>
#include <vector>
#include <set>
#include <list>
using std::string;
using std::vector;
using std::list;

class AmPluginFactory;
class AmSessionFactory;
class AmSessionEventHandlerFactory;
class AmDynInvokeFactory;
class AmLoggingFacility;
class AmSipRequest;
struct SdpPayload;

struct amci_exports_t;
struct amci_codec_t;
struct amci_payload_t;
struct amci_inoutfmt_t;
struct amci_subtype_t;

/** Interface that a payload provider needs to implement */
class AmPayloadProvider {
 public: 
  AmPayloadProvider() { }
  virtual ~AmPayloadProvider() { }
  
  /** 
   * Payload lookup function.
   * @param payload_id Payload ID.
   * @return NULL if failed .
   */
  virtual amci_payload_t*  payload(int payload_id) const = 0;

  /** 
   * Payload lookup function by name & rate
   * @param name Payload ID.
   * @return -1 if failed, else the internal payload id.
   */
  virtual int getDynPayload(const string& name, int rate, int encoding_param) const = 0;
  
  /**
   * List all the payloads available for a media type
   */
  virtual void getPayloads(vector<SdpPayload>& pl_vec) const = 0;
};

/**
 * \brief Container for loaded Plug-ins.
 */
class AmPlugIn : public AmPayloadProvider
{
 private:
  static AmPlugIn* _instance;

  std::set<string>                  rtld_global_plugins;
  vector<void*> dlls;

  std::map<int,amci_codec_t*>       codecs;
  std::map<int,amci_payload_t*>     payloads;
  std::multimap<int,int>            payload_order;
  std::map<string,amci_inoutfmt_t*> file_formats;

  std::map<string,AmSessionFactory*>             name2app;
  AmMutex name2app_mut;

  std::map<string,AmSessionEventHandlerFactory*> name2seh;
  std::map<string,AmPluginFactory*>              name2base;
  std::map<string,AmDynInvokeFactory*>           name2di;
  std::map<string,AmLoggingFacility*>            name2logfac;

  std::map<string,AmPluginFactory*>             module_objects;

  //AmCtrlInterfaceFactory *ctrlIface;

  int dynamic_pl; // range: 96->127, see RFC 1890
  std::set<string> excluded_payloads;  // don't load these payloads (named)
    
  AmPlugIn();
  virtual ~AmPlugIn();

  /** @return -1 if failed, else 0. */
  int loadPlugIn(const string& file, const string& plugin_name, vector<AmPluginFactory*>& plugins);

  int loadAudioPlugIn(amci_exports_t* exports);
  int loadAppPlugIn(AmPluginFactory* cb);
  int loadSehPlugIn(AmPluginFactory* cb);
  int loadBasePlugIn(AmPluginFactory* cb);
  int loadDiPlugIn(AmPluginFactory* cb);
  int loadLogFacPlugIn(AmPluginFactory* f);

  int initLoggingModules();
 public:

  int addCodec(amci_codec_t* c);
  int addPayload(amci_payload_t* p);
  int addFileFormat(amci_inoutfmt_t* f);

  void set_load_rtld_global(const string& plugin_name);

  static AmPlugIn* instance();
  static void dispose();

  void init();

  /** 
   * Loads all plug-ins from the directory given as parameter. 
   * @return -1 if failed, else 0.
   */
  int load(const string& directory, const string& plugins);

  /** register logging plugins to receive logging messages */
  void registerLoggingPlugins();

  /** 
   * Payload lookup function.
   * @param payload_id Payload ID.
   * @return NULL if failed .
   */
  amci_payload_t*  payload(int payload_id) const;

  /** 
   * Payload lookup function by name & rate
   * @param name Payload ID.
   * @return -1 if failed, else the internal payload id.
   */
  int getDynPayload(const string& name, int rate, int encoding_param) const;

  /** return 0, or -1 in case of error. */
  void getPayloads(vector<SdpPayload>& pl_vec) const;

  /** @return the suported payloads. */

  /** @return the order of payloads. */

  /** 
   * File format lookup according to the 
   * format name and/or file extension.
   * @param fmt_name Format name.
   * @param ext File extension.
   * @return NULL if failed.
   */
  amci_inoutfmt_t* fileFormat(const string& fmt_name, const string& ext = "");

  /** 
   * File format's subtype lookup function.
   * @param iofmt The file format.
   * @param subtype Subtype ID (see plug-in declaration for values).
   * @return NULL if failed.
   */
  amci_subtype_t*  subtype(amci_inoutfmt_t* iofmt, int subtype);

  /** 
   * File subtype lookup function.
   * @param subtype_name The subtype's name (e.g. Pcm16).
   * @return NULL if failed.
   */
  amci_subtype_t* subtype(amci_inoutfmt_t* iofmt, const string& subtype_name);

  /** 
   * Codec lookup function.
   * @param id Codec ID (see amci/codecs.h).
   * @return NULL if failed.
   */
  amci_codec_t*    codec(int id);

  /** 
   * get codec format parameters
   * @param id Codec ID (see amci/codecs.h).
   * @param is_offer for an offer?
   * @param fmt_params_in input parameters for an answer
   * @return fmt parameters for SDP (offer or answer)
   */
  string getSdpFormatParameters(int codec_id, bool is_offer, const string& fmt_params_in);



  /**
   * Application lookup function
   * @param app_name application name
   * @return NULL if failed (-> application not found).
   */
  AmSessionFactory* getFactory4App(const string& app_name);

  /** @return true if this record has been inserted. */
  bool registerFactory4App(const string& app_name, AmSessionFactory* f);

  /** register a factory for applications
      @return true on success
   */
  static bool registerApplication(const string& app_name, AmSessionFactory* f);

  /** register a SIP Event Handler module
      note: unprotected, use only at server startup (onLoad)
      @return true on success
   */
  static bool registerSIPEventHandler(const string& seh_name,
				      AmSessionEventHandlerFactory* f);
  /** register a DI Interface module
      note: unprotected, use only at server startup (onLoad)
      @return true on success
   */
  static bool registerDIInterface(const string& di_name, AmDynInvokeFactory* f);

  /** register a logging facility
      note: unprotected, use only at server startup (onLoad)
      @return true on success
   */
  static bool registerLoggingFacility(const string& lf_name, AmLoggingFacility* f);

  /**
   * Find the proper SessionFactory
   * for the given request.
   */
  AmSessionFactory* findSessionFactory(const AmSipRequest& req, string& app_name);

  /**
   * Session event handler lookup function
   * @param name application name
   * @return NULL if failed (-> handler not found).
   */
  AmSessionEventHandlerFactory* getFactory4Seh(const string& name);

  /**
   * Dynamic invokation component
   */
  AmDynInvokeFactory* getFactory4Di(const string& name);

  /**
   * logging facility lookup function
   * @param name application name
   * @return NULL if failed (-> handler not found).
   */
  AmLoggingFacility* getFactory4LogFaclty(const string& name);

};

#endif
