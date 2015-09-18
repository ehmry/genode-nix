include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx nixstore jitterentropy

SRC_CC = shared.cc dummy.cc

INC_DIR += $(NIX_DIR)/libutil

vpath %.cc $(NIX_DIR)/libmain
vpath %.cc $(REP_DIR)/src/lib/nixmain

SHARED_LIB = yes