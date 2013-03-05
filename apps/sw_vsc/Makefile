plug_in_name = sw_vsc

module_ldflags = -lmysqlclient `pcre-config --libs` -lpcrecpp
module_cflags  = -I/usr/include/mysql `pcre-config --cflags`
module_extra_objs =

extra_install = $(plug_in_name)_audio

COREPATH ?=../../core
include $(COREPATH)/plug-in/Makefile.app_module
