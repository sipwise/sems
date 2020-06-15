plug_in_name = routing
sbc_app_path = $(SEMSPATH)/apps/sbc

module_ldflags = -lboost_regex
module_cflags  = -DMOD_NAME=\"$(plug_in_name)\" -I$(sbc_app_path)
COREPATH = $(SEMSPATH)/core
include $(COREPATH)/plug-in/Makefile.app_module

