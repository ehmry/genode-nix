##
## Makefile for generating library metatada
##

INSTALL_DIR    := $(CURDIR)/out

export BASE_DIR         ?= /genode/repos/base
export VERBOSE          ?= @
export VERBOSE_DIR      ?= --no-print-directory
export VERBOSE_MK       ?= @
export ECHO             ?= echo -e

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

LIB_MK_DIRS = $(foreach REP,$(REPOSITORIES),$(addprefix $(REP)/lib/mk/spec/,$(SPECS)) $(REP)/lib/mk)

#
# Of all possible file locations, use the (first) one that actually exists.
#
LIB_MK = $(firstword $(wildcard $(addsuffix /$(LIB).mk,$(LIB_MK_DIRS))))

ifeq ($(LIB_MK),)
$(error "Library-description file $(DARK_COL)$(LIB).mk$(DEFAULT_COL) is missing")
endif

override REP_DIR := $(firstword $(foreach REP,$(REPOSITORIES),$(findstring $(REP)/,$(LIB_MK))))
override REP_DIR := $(REP_DIR:/=)

include $(LIB_MK)

ifdef SHARED_LIB
NIX_ATTRS :=  {deps=[$(foreach L,$(sort $(LIBS)),"$(L)")];sharedLib=true;}
else
NIX_ATTRS :=  {deps=[$(foreach L,$(sort $(LIBS)),"$(L)")];}
endif

all: /ingest/out

/ingest/out:
	echo '$(NIX_ATTRS)' >$@

#
#
#
