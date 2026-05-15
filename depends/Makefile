SHELL := /bin/sh

depends_dir := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
repo_dir := $(abspath $(depends_dir)/..)
sources_dir := $(depends_dir)/sources
work_dir := $(depends_dir)/work
build_dir := $(work_dir)/build
stamp_dir := $(work_dir)/stamps

CC ?= cc
AR ?= ar
RANLIB ?= ranlib
CURL ?= curl
TAR ?= tar
INSTALL ?= install
CFLAGS ?= -O2
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
HOST ?= $(shell $(CC) -dumpmachine 2>/dev/null || uname -s | tr A-Z a-z)-$(shell uname -m)
PREFIX ?= $(depends_dir)/$(HOST)
SHA256SUM := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo "shasum -a 256")

ifneq (,$(findstring darwin,$(HOST)))
macos_deployment_target := $(shell test -f '$(repo_dir)/.macos-deployment-target' && cat '$(repo_dir)/.macos-deployment-target')
ifneq ($(macos_deployment_target),)
ifeq ($(origin MACOSX_DEPLOYMENT_TARGET), undefined)
MACOSX_DEPLOYMENT_TARGET := $(macos_deployment_target)
endif
export MACOSX_DEPLOYMENT_TARGET
CFLAGS += -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
endif
endif

include $(depends_dir)/packages/miniupnpc.mk
include $(depends_dir)/packages/libnatpmp.mk

.PHONY: all print-prefix clean distclean miniupnpc libnatpmp

all: miniupnpc libnatpmp $(depends_dir)/current-prefix.env

print-prefix:
	@printf '%s\n' '$(PREFIX)'

miniupnpc: $(stamp_dir)/miniupnpc-$(miniupnpc_version).installed
libnatpmp: $(stamp_dir)/libnatpmp-$(libnatpmp_version).installed

$(sources_dir):
	mkdir -p '$@'

$(build_dir):
	mkdir -p '$@'

$(stamp_dir):
	mkdir -p '$@'

$(sources_dir)/$(miniupnpc_file): | $(sources_dir)
	$(CURL) -fL --retry 3 --connect-timeout 20 -o '$@' '$(miniupnpc_url)'
	printf '%s  %s\n' '$(miniupnpc_sha256)' '$@' | $(SHA256SUM) -c -

$(sources_dir)/$(libnatpmp_file): | $(sources_dir)
	$(CURL) -fL --retry 3 --connect-timeout 20 -o '$@' '$(libnatpmp_url)'
	printf '%s  %s\n' '$(libnatpmp_sha256)' '$@' | $(SHA256SUM) -c -

$(stamp_dir)/miniupnpc-$(miniupnpc_version).extracted: $(sources_dir)/$(miniupnpc_file) | $(build_dir) $(stamp_dir)
	rm -rf '$(build_dir)/$(miniupnpc_dir)'
	$(TAR) -C '$(build_dir)' -xzf '$<'
	touch '$@'

$(stamp_dir)/libnatpmp-$(libnatpmp_version).extracted: $(sources_dir)/$(libnatpmp_file) | $(build_dir) $(stamp_dir)
	rm -rf '$(build_dir)/$(libnatpmp_dir)'
	$(TAR) -C '$(build_dir)' -xzf '$<'
	touch '$@'

$(stamp_dir)/miniupnpc-$(miniupnpc_version).installed: $(stamp_dir)/miniupnpc-$(miniupnpc_version).extracted
	$(MAKE) -C '$(build_dir)/$(miniupnpc_dir)' \
		CC='$(CC)' AR='$(AR)' RANLIB='$(RANLIB)' CFLAGS='$(CFLAGS)' \
		-j$(JOBS) build/libminiupnpc.a
	mkdir -p '$(PREFIX)/include/miniupnpc' '$(PREFIX)/lib/pkgconfig'
	$(INSTALL) -m 644 '$(build_dir)/$(miniupnpc_dir)'/include/*.h '$(PREFIX)/include/miniupnpc/'
	$(INSTALL) -m 644 '$(build_dir)/$(miniupnpc_dir)/build/libminiupnpc.a' '$(PREFIX)/lib/'
	{ \
	  printf 'prefix=%s\n' '$(PREFIX)'; \
	  printf 'exec_prefix=$${prefix}\n'; \
	  printf 'libdir=$${exec_prefix}/lib\n'; \
	  printf 'includedir=$${prefix}/include\n\n'; \
	  printf 'Name: miniUPnPc\n'; \
	  printf 'Description: UPnP IGD client lightweight library\n'; \
	  printf 'URL: https://miniupnp.tuxfamily.org/\n'; \
	  printf 'Version: %s\n' '$(miniupnpc_version)'; \
	  printf 'Libs: -L$${libdir} -lminiupnpc\n'; \
	  printf 'Cflags: -I$${includedir}\n'; \
	} > '$(PREFIX)/lib/pkgconfig/miniupnpc.pc'
	touch '$@'

$(stamp_dir)/libnatpmp-$(libnatpmp_version).installed: $(stamp_dir)/libnatpmp-$(libnatpmp_version).extracted
	$(MAKE) -C '$(build_dir)/$(libnatpmp_dir)' \
		CC='$(CC)' AR='$(AR)' RANLIB='$(RANLIB)' CFLAGS='$(CFLAGS)' \
		-j$(JOBS) libnatpmp.a
	mkdir -p '$(PREFIX)/include' '$(PREFIX)/lib/pkgconfig'
	$(INSTALL) -m 644 '$(build_dir)/$(libnatpmp_dir)/natpmp.h' '$(PREFIX)/include/'
	$(INSTALL) -m 644 '$(build_dir)/$(libnatpmp_dir)/natpmp_declspec.h' '$(PREFIX)/include/'
	$(INSTALL) -m 644 '$(build_dir)/$(libnatpmp_dir)/libnatpmp.a' '$(PREFIX)/lib/'
	{ \
	  printf 'prefix=%s\n' '$(PREFIX)'; \
	  printf 'exec_prefix=$${prefix}\n'; \
	  printf 'libdir=$${exec_prefix}/lib\n'; \
	  printf 'includedir=$${prefix}/include\n\n'; \
	  printf 'Name: libnatpmp\n'; \
	  printf 'Description: NAT-PMP lightweight library\n'; \
	  printf 'URL: https://miniupnp.tuxfamily.org/\n'; \
	  printf 'Version: %s\n' '$(libnatpmp_version)'; \
	  printf 'Libs: -L$${libdir} -lnatpmp\n'; \
	  printf 'Cflags: -I$${includedir}\n'; \
	} > '$(PREFIX)/lib/pkgconfig/libnatpmp.pc'
	touch '$@'

$(depends_dir)/current-prefix.env: $(stamp_dir)/miniupnpc-$(miniupnpc_version).installed $(stamp_dir)/libnatpmp-$(libnatpmp_version).installed
	printf 'DINERO_DEPENDS_PREFIX=%s\n' '$(PREFIX)' > '$@'
	rm -f '$(depends_dir)/current'
	ln -s '$(notdir $(PREFIX))' '$(depends_dir)/current' 2>/dev/null || true

clean:
	rm -rf '$(work_dir)'

distclean: clean
	rm -rf '$(sources_dir)' '$(PREFIX)' '$(depends_dir)/current' '$(depends_dir)/current-prefix.env'
