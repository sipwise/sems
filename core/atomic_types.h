#ifndef _atomic_types_h_
#define _atomic_types_h_

#include <assert.h>
#include "log.h"

#include <atomic>

using std::atomic_int;


class atomic_ref_cnt;
void inc_ref(atomic_ref_cnt* rc);
void dec_ref(atomic_ref_cnt* rc);

class atomic_ref_cnt
{
  atomic_int ref_cnt;

protected:
  atomic_ref_cnt() {}

  void _inc_ref() { ++ref_cnt; }
  bool _dec_ref() { return --ref_cnt == 0; }

  virtual ~atomic_ref_cnt() {}
  virtual void on_destroy() {}

  friend void inc_ref(atomic_ref_cnt* rc);
  friend void dec_ref(atomic_ref_cnt* rc);
};

inline void inc_ref(atomic_ref_cnt* rc)
{
  assert(rc);
  rc->_inc_ref();
}

inline void dec_ref(atomic_ref_cnt* rc)
{
  assert(rc);
  if(rc->_dec_ref()){
    rc->on_destroy();
    delete rc;
  }
}


#endif
