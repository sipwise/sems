#ifndef _SdNotify_h_
#define _SdNotify_h_

#include <string>
#include <mutex>
#include <condition_variable>

class SdNotifier
{
public:
      void ready();
      void stopping();
      void status(const std::string &s);

      void waiter(); // increases waiter count by one
      void running(); // decreases waiter count by one

private:
      std::mutex _lock;
      std::condition_variable _cond;
      unsigned int waiters = 0;
};

#endif // _SdNotify_h_

