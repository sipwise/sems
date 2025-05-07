
#include "XmlRpcSource.h"
#include "XmlRpcSocket.h"
#include "XmlRpcUtil.h"

namespace XmlRpc {


  XmlRpcSource::XmlRpcSource(int fd /*= -1*/, bool deleteOnClose /*= false*/) 
    : _ssl(false), _ssl_ctx(NULL), _ssl_ssl(NULL), _ssl_meth(NULL),
      _fd(fd), _deleteOnClose(deleteOnClose), _keepOpen(false)      
  {
  }

  XmlRpcSource::~XmlRpcSource()
  {
  }


  void
  XmlRpcSource::close()
  {
    if (_fd != -1) {
      XmlRpcUtil::log(2,"XmlRpcSource::close: closing socket %d.", _fd);
      XmlRpcSocket::close(_fd);
      XmlRpcUtil::log(2,"XmlRpcSource::close: done closing socket %d.", _fd);
      _fd = -1;
    }
    if (_ssl_ssl != (SSL *) NULL) {
      SSL_shutdown (_ssl_ssl);
      SSL_free (_ssl_ssl);
      SSL_CTX_free (_ssl_ctx);
    }
    if (_deleteOnClose) {
      XmlRpcUtil::log(2,"XmlRpcSource::close: deleting this");
      _deleteOnClose = false;
      delete this; /* the one who is the last turns off the light */
    }
  }

} // namespace XmlRpc
