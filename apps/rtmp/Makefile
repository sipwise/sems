
plug_in_name = rtmp

module_cflags  = -DNO_CRYPTO
module_ldflags = -lspeex -lrtmp

extra_target = sono
extra_clean = clean_sono

COREPATH ?=../../core
include $(COREPATH)/plug-in/Makefile.app_module

#CXXFLAGS=-g -I/opt/local/include -DNO_CRYPTO
#LDFLAGS=-lstdc++ -g -L/opt/local/lib -lspeex -lpthread

.PHONY: sono
sono:
	cd flash_phone; $(MAKE)

.PHONY: clean_sono
clean_sono:
	cd flash_phone; $(MAKE) clean
