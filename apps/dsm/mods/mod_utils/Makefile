plug_in_name = mod_utils

DSMPATH ?= ../..

module_ldflags = -lssl -lcrypto
module_cflags  = -DMOD_NAME=\"$(plug_in_name)\" -I$(DSMPATH)

COREPATH ?=$(DSMPATH)/../../core
lib_full_name = $(DSMPATH)/mods/lib/$(lib_name)
include $(DSMPATH)/mods/Makefile.dsm_module
