# makefile written for gnu make
CXX		= /usr/local/gcc-3.1/bin/g++ 
SRC		= ../src
CPPFLAGS	= -I$(SRC)
DEBUG		= -g
#OPTIMIZE	= -O2
GCCWARN		= -Wall -Wstrict-prototypes
CXXFLAGS	= -std=c++20 $(DEBUG) $(GCCWARN) $(OPTIMIZE) $(INCLUDES)

LIB		= ../libXmlRpc.a -lssl

# Add your system-dependent network libs here
# Solaris: -lsocket -lnsl
SYSTEMLIBS	=

LDLIBS		= $(SYSTEMLIBS) $(LIB)

TESTS		= HelloClient HelloServer TestBase64Client TestBase64Server TestValues TestXml Validator TestEGroupwareSSLClient

all:		$(TESTS)

$(TESTS):	$(LIB)

clean:
		rm -f *.o
		rm -f $(TESTS)
