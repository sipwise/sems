COREPATH=..
include ../../Makefile.defs

$(info exclude_core_modules: $(exclude_core_modules))
core_modules = $(filter-out $(subst ;, ,$(exclude_core_modules)) \
		$(wildcard Makefile*) CMakeLists.txt, \
		$(wildcard *) )

.DEFAULT_GOAL := all
.PHONY: all
all: modules

.PHONY: clean
clean:
	@rm -f *.so
	@set -e; \
	for r in $(core_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			$(MAKE) -C $$r clean ; \
		fi ; \
	done

.PHONY: modules
modules:
	@set -e; \
	for r in $(core_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo  "" ; \
			echo  "" ; \
			$(MAKE) -C $$r all; \
		fi ; \
	done 

.PHONY: install
install: modules
	@set -e; \
	for r in $(core_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			$(MAKE) -C $$r install; \
		fi ; \
	done

.PHONY: install-cfg
install-cfg: $(DESTDIR)$(cfg_target)
	@set -e; \
	for r in $(core_modules) "" ; do \
		if [ -n "$$r" ]; then \
			echo "" ; \
			echo "" ; \
			COREPATH=../$(COREPATH) $(MAKE) -C $$r install-cfg; \
		fi ; \
	done
