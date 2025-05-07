
#ifndef _XMLRPCSOURCE_H_
#define _XMLRPCSOURCE_H_
//
// XmlRpc++ Copyright (c) 2002-2003 by Chris Morley
//
#if defined(_MSC_VER)
# pragma warning(disable:4786)    // identifier was truncated in debug info
#endif

// Deal with SSL dependencies
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace XmlRpc {

  //! An RPC source represents a file descriptor to monitor
  class XmlRpcSource {
  public:
    //! Constructor
    //!  @param fd The socket file descriptor to monitor.
    //!  @param deleteOnClose If true, the object deletes itself when close is called.
    XmlRpcSource(int fd = -1, bool deleteOnClose = false);

    //! Destructor
    virtual ~XmlRpcSource();

    //! Return the file descriptor being monitored.
    int getfd() const { return _fd; }
    //! Specify the file descriptor to monitor.
    void setfd(int fd) { _fd = fd; }

    //! Return whether the file descriptor should be kept open if it is no longer monitored.
    bool getKeepOpen() const { return _keepOpen; }
    //! Specify whether the file descriptor should be kept open if it is no longer monitored.
    void setKeepOpen(bool b=true) { _keepOpen = b; }

    //! Close the owned fd. If deleteOnClose was specified at construction, the object is deleted.
    virtual void close();

    bool getsDeletedOnClose() { return _deleteOnClose; }

    //! Return true to continue monitoring this source
    virtual unsigned handleEvent(unsigned eventType) = 0;

    // Keep track of SSL status and other such things
    bool _ssl;
    SSL_CTX* _ssl_ctx;
    SSL* _ssl_ssl;
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
    const SSL_METHOD* _ssl_meth;
#else
    SSL_METHOD* _ssl_meth;
#endif
  private:

    // Socket. This should really be a SOCKET (an alias for unsigned int*) on windows...
    int _fd;

    // In the server, a new source (XmlRpcServerConnection) is created
    // for each connected client. When each connection is closed, the
    // corresponding source object is deleted.
    bool _deleteOnClose;

    // In the client, keep connections open if you intend to make multiple calls.
    bool _keepOpen;
  };
} // namespace XmlRpc

#endif //_XMLRPCSOURCE_H_
