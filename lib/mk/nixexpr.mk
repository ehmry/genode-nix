include $(call select_from_repositories,lib/mk/nix-common.inc)

ifeq ($(findstring arm_v6, $(SPECS)), arm_v6)
CC_OPT += -DNIX_CPU=\"arm_v6-\"
else
ifeq ($(findstring arm_v6, $(SPECS)), arm_v6)
CC_OPT += -DNIX_CPU=\"arm_v7-\"
else
ifeq ($(findstring arm_v6, $(SPECS)), arm_v6)
CC_OPT += -DNIX_CPU=\"arm_v7a-\"
else
ifeq ($(findstring x86_32, $(SPECS)), x86_32)
CC_OPT += -DNIX_CPU=\"i686-\"
else
ifeq ($(findstring x86_64, $(SPECS)), x86_64)
CC_OPT += -DNIX_CPU=\"x86_64-\"
else
CC_OPT += -DNIX_CPU=\"unknown-\"
endif
endif
endif
endif
endif

ifeq ($(findstring linux, $(SPECS)), linux)
CC_OPT += -DNIX_KERNEL=\"linux-\"
else
ifeq ($(findstring nova, $(SPECS)), nova)
CC_OPT += -DNIX_KERNEL=\"nova-\"
else
CC_OPT += -DNIX_KERNEL=\"\"
endif
endif

LIBS += stdcxx nixutil nixstore

SRC_CC = \
	attr-set.cc attr-path.cc common-opts.cc eval.cc get-drvs.cc \
	json-to-value.cc lexer-tab.cc names.cc nixexpr.cc \
	parser-tab.cc primops.cc value-to-json.cc value-to-xml.cc

INC_DIR += $(NIX_DIR)/libexpr
INC_DIR += $(NIX_DIR)/libmain

vpath %.cc $(NIX_DIR)/libexpr

SHARED_LIB = yes
