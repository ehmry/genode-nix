include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx vfs nixformat

SRC_CC = affinity.cc archive.cc hash.cc regex.cc serialise.cc util.cc xml-writer.cc
SRC_C  = md5.c sha1.c sha256.c

vpath %.cc $(NIX_DIR)/libutil
vpath %.c $(NIX_DIR)/libutil

SHARED_LIB = yes