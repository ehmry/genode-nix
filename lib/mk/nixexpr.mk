include $(call select_from_repositories,lib/mk/nix-common.inc)

LIBS += stdcxx nixutil nixstore

SRC_CC = \
	attr-path.cc common-opts.cc eval.cc get-drvs.cc \
	json-to-value.cc lexer-tab.cc names.cc nixexpr.cc \
	parser-tab.cc primops.cc value-to-json.cc value-to-xml.cc

INC_DIR += $(NIX_DIR)/libexpr

CC_OPT += -DVERSION=\"1.8\"

vpath %.cc $(NIX_DIR)/libexpr

SHARED_LIB = yes
