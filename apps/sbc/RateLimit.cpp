#include "RateLimit.h"
#include "AmAppTimer.h"
#include <sys/time.h>

#define min(a,b) ((a) < (b) ? (a) : (b))

DynRateLimit::DynRateLimit(unsigned int _time_base_ms)
  : last_update(0), counter(0), time_base_ms(_time_base_ms)
{
}

bool DynRateLimit::limit(unsigned int rate, unsigned int peak, 
			 unsigned int size)
{
  lock();

  if(wall_clock_ms() - last_update
     > time_base_ms) {

    update_limit(rate,peak);
  }

  if(counter <= 0) {
    unlock();
    return true; // limit reached
  }

  counter -= size;
  unlock();

  return false; // do not limit
}

void DynRateLimit::update_limit(int rate, int peak)
{
  counter = min(peak, counter+rate);
  last_update = wall_clock_ms();
}

u_int32_t DynRateLimit::wall_clock_ms()
{
  struct timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec * 1000 + now.tv_usec / 1000;
}
