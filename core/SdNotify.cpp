#include "SdNotify.h"

#include <systemd/sd-daemon.h>

void
SdNotifier::ready()
{
      std::unique_lock<std::mutex> _l(_lock);
      while (waiters)
	    _cond.wait(_l);
      sd_notify(0, "READY=1");
}

void
SdNotifier::stopping()
{
      sd_notify(0, "STOPPING=1");
}


void
SdNotifier::status(const std::string &s)
{
      sd_notifyf(0, "STATUS=%s\n", s.c_str());
}

void
SdNotifier::waiter()
{
      std::lock_guard<std::mutex> _l(_lock);
      waiters++;
}

void
SdNotifier::running()
{
      std::lock_guard<std::mutex> _l(_lock);
      waiters--;
      if (!waiters)
	    _cond.notify_one();
}
