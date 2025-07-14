#include "async_file_writer.h"

async_file_writer::async_file_writer()
{
  evbase = event_base_new();

  // fake event to prevent the event loop from exiting
  ev_default = event_new(evbase,-1,EV_READ|EV_PERSIST,NULL,NULL);
  event_add(ev_default,NULL);
}

async_file_writer::~async_file_writer()
{
  event_free(ev_default);
  event_base_free(evbase);
}

void async_file_writer::start()
{
  event_add(ev_default,NULL);
  AmThread::start();
}

void async_file_writer::on_stop()
{
  event_del(ev_default);
  event_base_loopexit(evbase,NULL);
}

void async_file_writer::run()
{
  /* Start the event loop. */
  event_base_dispatch(evbase);
}
