#ifndef _CC_ACC_H
#define _CC_ACC_H

#include "AmApi.h"

/**
 * accounting class for calling card.
 * this module executes accounting functions
 * over XMLRPC.
 */
class CCAcc : public AmDynInvoke

{
  /** returns credit for pin, -1 if pin wrong */
  long getCredit(string pin);
  /** returns remaining credit */
  long subtractCredit(string pin, long amount);

  static CCAcc* _instance;
 public:
  CCAcc();
  ~CCAcc();
  static CCAcc* instance();
  void invoke(const string& method, const AmArg& args, AmArg& ret);
};

#endif
