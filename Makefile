# Thin wrapper so `make` works the same here as in the sibling Haiku ports.
# The real build system is meson: each vendored library under lv2stack/ builds
# with its own unmodified meson.build into a shared static stage prefix
# (build/stage), chained via PKG_CONFIG_PATH in dependency order
# lv2 -> zix -> serd -> sord -> sratom -> lilv. Our own host and plugin are
# small enough to compile with direct rules against the stage.
#
# Typical loop from the dev host:
#   rsync -a --delete --exclude build --exclude .git ./ haikulaptop:LV2-haiku/
#   ssh haikulaptop 'cd LV2-haiku && make'

BUILD_DIR ?= build
STAGE := $(abspath $(BUILD_DIR)/stage)
LV2_INSTALL_DIR ?= $(HOME)/config/non-packaged/add-ons/lv2

# Haiku-native default plugin search path (mirrors the sibling VST3 port's
# add-ons convention); set at build time through lilv's default_lv2_path
# option so no upstream file is patched. LV2_PATH at runtime still overrides.
DEFAULT_LV2_PATH ?= ~/.lv2:/boot/home/config/non-packaged/add-ons/lv2:/boot/system/non-packaged/add-ons/lv2:/boot/system/add-ons/lv2

MESON ?= meson
NINJA ?= ninja
PKGCFG := PKG_CONFIG_PATH=$(STAGE)/lib/pkgconfig

MESON_COMMON = --prefix=$(STAGE) --libdir=lib --default-library=static \
	--buildtype=release -Ddocs=disabled -Dlint=false -Dtests=disabled

# Per-library options (verified against each lv2stack/<lib>/meson_options.txt).
# lv2: old_headers installs the <lv2/...> URI-style include layout the hosts
# use; lv2dir is set explicitly because upstream's Haiku default is the
# obsolete pre-package-management /boot/common/add-ons/lv2.
MESON_OPTS_lv2 = -Dtools=disabled -Dold_headers=true -Dbundles=true \
	-Dlv2dir=$(STAGE)/lib/lv2
MESON_OPTS_zix = -Dtests_cpp=disabled -Dbenchmarks=disabled
MESON_OPTS_serd = -Dtools=disabled -Dman=disabled
MESON_OPTS_sord = -Dtools=disabled -Dman=disabled -Dbindings_cpp=disabled
MESON_OPTS_sratom =
MESON_OPTS_lilv = -Dtools=enabled -Dbindings_py=disabled -Dbindings_cpp=disabled \
	-Ddynmanifest=disabled "-Ddefault_lv2_path=$(DEFAULT_LV2_PATH)"

CC ?= gcc
WARN = -Wall -Wextra
UNAME := $(shell uname)

ifeq ($(UNAME),Haiku)
# The laptop has the JACK runtime from the release hpkg but no headers; the
# API headers come from the sibling JACK2 port tree (the exact source of the
# installed libjack).
JACK_PORT_DIR ?= $(HOME)/jack-port-haiku
JACK_CFLAGS ?= -I$(JACK_PORT_DIR)/common
JACK_LIBS ?= -ljack
HOST_EXTRA_LIBS =
else
JACK_CFLAGS := $(shell pkg-config --cflags jack 2>/dev/null)
JACK_LIBS := $(shell pkg-config --libs jack 2>/dev/null)
HOST_EXTRA_LIBS = -lpthread -lm -ldl
endif

HKAMP_SRC = plugins/hkamp.lv2
HKAMP_OUT = $(BUILD_DIR)/hkamp.lv2

all: build

build: stack $(BUILD_DIR)/bin/lv2jackhost $(HKAMP_OUT)/hkamp.so

# ---- vendored stack (meson, unmodified upstream) --------------------------

stack: $(BUILD_DIR)/.stamp-lilv

$(BUILD_DIR)/.stamp-lv2:
$(BUILD_DIR)/.stamp-zix: $(BUILD_DIR)/.stamp-lv2
$(BUILD_DIR)/.stamp-serd: $(BUILD_DIR)/.stamp-zix
$(BUILD_DIR)/.stamp-sord: $(BUILD_DIR)/.stamp-serd
$(BUILD_DIR)/.stamp-sratom: $(BUILD_DIR)/.stamp-sord
$(BUILD_DIR)/.stamp-lilv: $(BUILD_DIR)/.stamp-sratom

$(BUILD_DIR)/.stamp-%:
	@if [ ! -f $(BUILD_DIR)/$*/build.ninja ]; then \
		$(PKGCFG) $(MESON) setup $(BUILD_DIR)/$* lv2stack/$* \
			$(MESON_COMMON) $(MESON_OPTS_$*); \
	fi
	$(PKGCFG) $(MESON) install -C $(BUILD_DIR)/$*
	@touch $@

# ---- our own code (direct rules against the stage) ------------------------

$(BUILD_DIR)/bin/lv2jackhost: hosts/lv2jackhost/lv2jackhost.c $(BUILD_DIR)/.stamp-lilv
	@mkdir -p $(BUILD_DIR)/bin
	$(CC) -std=c99 $(WARN) -O2 -g -o $@ $< \
		`$(PKGCFG) pkg-config --cflags lilv-0 zix-0 lv2` $(JACK_CFLAGS) \
		`$(PKGCFG) pkg-config --static --libs lilv-0 zix-0` \
		$(JACK_LIBS) $(HOST_EXTRA_LIBS)

$(HKAMP_OUT)/hkamp.so: $(HKAMP_SRC)/hkamp.c $(BUILD_DIR)/.stamp-lv2
	@mkdir -p $(HKAMP_OUT)
	$(CC) -std=c99 $(WARN) -O2 -fPIC -shared -o $@ $< \
		`$(PKGCFG) pkg-config --cflags lv2` -lm
	cp $(HKAMP_SRC)/manifest.ttl $(HKAMP_SRC)/hkamp.ttl $(HKAMP_OUT)/

# ---- install / validate ----------------------------------------------------

# On Haiku only: install build prerequisites.
deps:
	pkgman install -y meson ninja pkgconfig

# The LV2 specification bundles must be discoverable on LV2_PATH for lilv to
# resolve plugin metadata.
install-specs: stack
	mkdir -p $(LV2_INSTALL_DIR)
	@for b in $(STAGE)/lib/lv2/*.lv2; do \
		[ -d "$$b" ] || continue; \
		echo "install $$b -> $(LV2_INSTALL_DIR)/$$(basename $$b)"; \
		rm -rf "$(LV2_INSTALL_DIR)/$$(basename $$b)"; \
		cp -r "$$b" "$(LV2_INSTALL_DIR)/"; \
	done

install: $(HKAMP_OUT)/hkamp.so
	mkdir -p $(LV2_INSTALL_DIR)
	rm -rf $(LV2_INSTALL_DIR)/hkamp.lv2
	cp -r $(HKAMP_OUT) $(LV2_INSTALL_DIR)/hkamp.lv2

validate: stack
	@echo "== lv2ls =="
	$(STAGE)/bin/lv2ls
	@echo "== lv2info hkamp =="
	$(STAGE)/bin/lv2info https://github.com/rations/LV2-haiku/plugins/hkamp

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all build stack deps install install-specs validate clean
