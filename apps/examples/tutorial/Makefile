COREPATH ?= ../../../core
.DEFAULT_GOAL := all

include $(COREPATH)/../Makefile.defs

exclude_modules ?= 
modules = $(filter-out $(exclude_modules) \
		$(wildcard Makefile*) $(wildcard Readme*) CVS, \
		$(wildcard *) )

.PHONY: all
all: modules

.PHONY: clean
clean:
	@rm -f *.so
	@set -e; \
	for r in $(modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r clean ; \
		fi ; \
	done

.PHONY: modules
modules:
	@set -e; \
	for r in $(modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo  "" ; \
			echo  "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r all; \
		fi ; \
	done 

.PHONY: install
install: install-bin

.PHONY: install-bin
install-bin: modules
	@set -e; \
	for r in $(modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install; \
		fi ; \
	done

.PHONY: install-cfg
install-cfg: $(DESTDIR)$(cfg_target)
	@set -e; \
	for r in $(modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install-cfg; \
		fi ; \
	done
