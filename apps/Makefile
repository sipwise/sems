COREPATH ?= ../core
.DEFAULT_GOAL := all

local_exclude_app_modules=confpin early_dbprompt mobile_push py_sems gateway twit

include $(COREPATH)/../Makefile.defs


# modules...

# if app_modules is set 
#   make $(app_modules)
# else 
#   if exclude_app_modules is set
#     make * - $(exclude_app_modules) + local excludes
#   else
#     make * - Makefile.defs:exclude_app_modules +  local excludes
#   

ifeq ($(app_modules),)

 ifeq ($(exclude_app_modules),)
  exclude_app_modules = ann_b2b announce_transfer announcement annrecorder callback callqueues conference confpin conf_auth diameter_client dimanagedha early_announce early_dbprompt examples examples/tutorial/cc_acc gateway ivr jsonrpc mailbox memcachedstore mobile_push mp3 msg_storage mwi pin_collect precoded_announce py_sems rtmp twit voicebox voicemail webconference
 endif

 ifeq ($(TEST_ENVIRONMENT),yes)
  exclude_app_modules = ann_b2b announce_transfer announcement annrecorder callback callqueues conference confpin conf_auth diameter_client dimanagedha early_announce early_dbprompt examples examples/tutorial/cc_acc gateway ivr jsonrpc mailbox memcachedstore mobile_push mp3 msg_storage mwi pin_collect precoded_announce py_sems rtmp twit voicebox voicemail webconference sw_rating
 endif

 exclude_modules = $(exclude_app_modules)
 exclude_modules += $(local_exclude_app_modules)

 ifneq ($(USE_MONITORING), yes)
 exclude_modules += monitoring
 endif

 $(info local_exclude_app_modules: $(local_exclude_app_modules))
 $(info exclude_app_modules: $(exclude_app_modules))
 app_modules = $(filter-out $(subst ;, ,$(exclude_modules)) \
		$(wildcard Makefile*) CVS CMakeLists.txt, \
		$(wildcard *) examples/tutorial/cc_acc )

endif


$(info app_modules: $(app_modules))

.PHONY: all
all: modules

.PHONY: clean
clean:
	@rm -f *.so
	@set -e; \
	for r in $(app_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "Cleaning $$r" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r clean ; \
		fi ; \
	done

.PHONY: modules
modules:
	@set -e; \
	for r in $(app_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo  "" ; \
			echo "Making $$r" ; \
			echo  "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r all; \
		fi ; \
	done 

.PHONY: install
install: install-bin

.PHONY: install-bin
install-bin: modules
	@set -e; \
	for r in $(app_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "Installing $$r binaries" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install; \
		fi ; \
	done

.PHONY: install-cfg
install-cfg: $(DESTDIR)$(cfg_target)
	@set -e; \
	for r in $(app_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "Installing $$r configuration" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install-cfg; \
		fi ; \
	done
