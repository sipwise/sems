#ifndef _async_file_writer_h_

#include "AmThread.h"
#include "singleton.h"

#include <event2/event.h>

class async_file_writer
  : public AmThread,
    public singleton<async_file_writer>
{
  friend class singleton<async_file_writer>;

  struct event_base* evbase;
  struct event*  ev_default;

protected:
  async_file_writer();
  ~async_file_writer();

  const char *identify() { return "async_file_writer"; }
  void on_stop();
  void run();
  
public:
  void start();

  event_base* get_evbase() const {
    return evbase;
  }
};

#endif
