#ifndef _singleton_h_
#define _singleton_h_

#include "AmThread.h"

template<class T>
class singleton
{
public:
  static T* instance()
  {
    inst_m.lock();
    if(NULL == _instance) {
      _instance = new T();
    }
    inst_m.unlock();

    return _instance;
  }

  static bool haveInstance()
  {
    bool res = false;
    inst_m.lock();
    res = _instance != NULL;
    inst_m.unlock();
    return res;
  }
  
  static void dispose() 
  {
    inst_m.lock();
    if(_instance != NULL){
      delete _instance;
      _instance = NULL;
    }
    inst_m.unlock();
  }

private:
  static T*       _instance;
  static AmMutex  inst_m;
};

template<class T>
T* singleton<T>::_instance = NULL;

template<class T>
AmMutex singleton<T>::inst_m;

#endif
