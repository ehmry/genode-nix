include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx blake2s nixutil curl

SRC_CC = build.cc builtins.cc derivations.cc download.cc  globals.cc misc.cc store-api.cc store.cc pathlocks.cc

INC_DIR += $(REP_DIR)/src/lib/nixstore $(NIX_DIR)/libstore $(NIX_DIR)/libutil

vpath %.cc $(REP_DIR)/src/lib/nixstore
vpath %.cc $(NIX_DIR)/libstore

SHARED_LIB = yes
