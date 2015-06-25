include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx

SRC_CC = format_implementation.cc free_funcs.cc parsing.cc

vpath %.cc $(NIX_DIR)/boost/format

SHARED_LIB = yes