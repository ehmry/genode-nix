include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx libc blake2s

SRC_CC = derivations.cc misc.cc globals.cc store-api.cc nichts_store.cc build.cc

INC_DIR += $(NIX_DIR)/libstore $(NIX_DIR)/libutil

vpath %.cc $(REP_DIR)/src/lib/nixstore
vpath %.cc $(NIX_DIR)/libstore

SHARED_LIB = yes
