include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx blake2s nixutil

SRC_CC = derivations.cc misc.cc globals.cc store-api.cc store.cc build.cc

INC_DIR += $(REP_DIR)/src/lib/nixstore $(NIX_DIR)/libstore $(NIX_DIR)/libutil

vpath %.cc $(NIX_DIR)/libstore
vpath %.cc $(REP_DIR)/src/lib/nixstore

SHARED_LIB = yes
