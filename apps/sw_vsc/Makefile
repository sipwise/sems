plug_in_name = sw_vsc

module_ldflags = -lmysqlclient `pkg-config --libs re2`
module_cflags  = -I/usr/include/mysql `pkg-config --cflags re2`
module_extra_objs =

extra_install = $(plug_in_name)_audio

COREPATH ?=../../core
include $(COREPATH)/plug-in/Makefile.app_module
