DSMPATH = ..
COREPATH = $(DSMPATH)/../../core
.DEFAULT_GOAL := all

include $(COREPATH)/../Makefile.defs

exclude_dsm_modules ?= mod_mysql mod_aws mod_curl mod_xml mod_py

ifeq ($(TEST_ENVIRONMENT), yes)
  exclude_dsm_modules += mod_swrating
endif

ifneq ($(USE_MONITORING), yes)
exclude_dsm_modules += mod_monitoring
endif

ifneq ($(WITH_ZRTP), yes)
exclude_dsm_modules += mod_zrtp
endif

$(info exclude_dsm_modules: $(exclude_dsm_modules))
dsm_modules = $(filter-out $(subst ;, ,$(exclude_dsm_modules))\
		$(wildcard Makefile*) lib CMakeLists.txt, \
		$(wildcard mod_*) )
$(info dsm_modules: $(dsm_modules))

.PHONY: all
all: modules

.PHONY: clean
clean:
	@rm -f lib/*.so
	@set -e; \
	for r in $(dsm_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r clean ; \
		fi ; \
	done

.PHONY: modules
modules:
	@set -e; \
	for r in $(dsm_modules) "" ; do \
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
	for r in $(dsm_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install; \
		fi ; \
	done

.PHONY: install-cfg
install-cfg: $(DESTDIR)$(cfg_target)
	@set -e; \
	for r in $(dsm_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install-cfg; \
		fi ; \
	done
