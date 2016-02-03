##
## Makefile for building libraries
##
## The following variables must be passed when calling this file:
##
##   CROSS_DEV_PREFIX
##   EXTERNAL_OBJECTS
##   LIB
##   REPOSITORIES
##


##
## Define global configuration variables
##

-include etc/build.conf

BUILD_BASE_DIR := $(CURDIR)
INSTALL_DIR    := /ingest

export BASE_DIR         ?= /genode/repos/base
export REPOSITORIES     ?= $(BASE_DIR:%base=%base-linux) $(BASE_DIR)
export VERBOSE_DIR      ?= --no-print-directory
export VERBOSE_MK       ?= @
export LIB_CACHE_DIR    ?= $(BUILD_BASE_DIR)/var/libcache
export LIB_PROGRESS_LOG ?= $(BUILD_BASE_DIR)/progress.log
export LIB_DEP_FILE     ?= var/libdeps
export ECHO             ?= echo -e
export CONTRIB_DIR

#
# Include common utility functions
#
include $(BASE_DIR)/mk/util.inc

#
# Load specs
#
-include $(call select_from_repositories,etc/specs.conf)
export SPEC_FILES := $(foreach SPEC,$(SPECS),$(call select_from_repositories,mk/spec/$(SPEC).mk))
include $(SPEC_FILES)
export SPECS

export LIBGCC_INC_DIR = $(shell dirname `$(CUSTOM_CXX_LIB) -print-libgcc-file-name`)/include

LIB_MK_DIRS = $(foreach REP,$(REPOSITORIES),$(addprefix $(REP)/lib/mk/spec/,$(SPECS)) $(REP)/lib/mk)

#
# Of all possible file locations, use the (first) one that actually exists.
#
LIB_MK = $(firstword $(wildcard $(addsuffix /$(LIB).mk,$(LIB_MK_DIRS))))

ifeq ($(LIB_MK),)
$(error "Library-description file $(DARK_COL)$(LIB).mk$(DEFAULT_COL) is missing LIB_MK_DIRS=$(LIB_MK_DIRS)")
endif

override REP_DIR := $(firstword $(foreach REP,$(REPOSITORIES),$(findstring $(REP)/,$(LIB_MK))))
override REP_DIR := $(REP_DIR:/=)

LIBGCC = $(shell $(CC) $(CC_MARCH) -print-libgcc-file-name)


include $(BASE_DIR)/mk/base-libs.mk
include $(LIB_MK)

include $(BASE_DIR)/mk/global.mk
include $(BASE_DIR)/mk/lib.mk

ifdef SHARED_LIBRARY
all: /ingest/$(LIB).lib.so
else
all: /ingest/$(LIB).lib.a
endif

/ingest/$(LIB).lib.a: $(LIB).lib.a
	cp $< $@

/ingest/$(LIB).lib.so: $(LIB).lib.so
	cp $< $@