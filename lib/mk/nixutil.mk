include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx vfs nixformat libcrypto

SRC_CC = affinity.cc archive.cc compression.cc hash.cc regex.cc serialise.cc util.cc xml-writer.cc

INC_DIR += $(NIX_DIR)/libutil

vpath %.cc $(REP_DIR)/src/lib/nixutil
vpath %.cc $(NIX_DIR)/libutil

SHARED_LIB = yes

