all: make_tools

COREPATH ?= ../core
include $(COREPATH)/../Makefile.defs

sems_tools = sems-logfile-callextract

install: install_tools

make_tools: $(sems_tools)

install_tools: make_tools $(DESTDIR)$(bin_prefix)/$(bin_dir)
	@set -e; \
	for r in $(sems_tools)  ; do \
		x=`echo $$r | sed s/sems-/$(APP_NAME)-/g` ; \
		echo "installing $$r -> $$x" ; \
		$(INSTALL_BIN) $$r $(DESTDIR)$(bin_prefix)/$(bin_dir)/$$x ; \
	done

sems-logfile-callextract: logfile-splitter.o
	$(LD) -o sems-logfile-callextract logfile-splitter.o -lstdc++

clean:
	rm -f logfile-splitter.o sems-logfile-callextract
