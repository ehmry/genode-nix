##
## Makefile for building binaries
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

TARGET_MK_DIRS = $(foreach REP,$(REPOSITORIES),$(addprefix $(REP)/src/$(BIN)/spec/,$(SPECS)) $(REP)/src/$(BIN))

#
# Of all possible file locations, use the (first) one that actually exists.
#
TARGET_MK = $(firstword $(wildcard $(addsuffix /target.mk,$(TARGET_MK_DIRS))))

ifeq ($(TARGET_MK),)
$(error "Library-description file $(DARK_COL)$(BIN).mk$(DEFAULT_COL) is missing")
endif

override REP_DIR := $(firstword $(foreach REP,$(REPOSITORIES),$(findstring $(REP)/,$(TARGET_MK))))
override REP_DIR := $(REP_DIR:/=)

LIBGCC = $(shell $(CC) $(CC_MARCH) -print-libgcc-file-name)
include $(BASE_DIR)/mk/global.mk

include $(TARGET_MK)

all: /ingest/$(NAME)

/ingest/$(NAME): $(NAME)
	cp $< $@
