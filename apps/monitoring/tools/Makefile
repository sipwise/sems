COREPATH_TOOLS ?= ../../../core
include $(COREPATH_TOOLS)/../Makefile.defs

monitoring_scripts = $(wildcard sems-*)

all: install_tools
install: install_tools

install_tools: $(DESTDIR)$(bin_prefix)/$(bin_dir)
	@set -e; \
	for r in $(monitoring_scripts)  ; do \
		x=`echo $$r | sed s/sems-/$(APP_NAME)-/g` ; \
		echo "installing $$r -> $$x" ; \
		$(INSTALL_BIN) $$r $(DESTDIR)$(bin_prefix)/$(bin_dir)/$$x ; \
	done


