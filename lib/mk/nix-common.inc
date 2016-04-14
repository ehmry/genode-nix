NIX_DIR := $(call select_from_ports,nix)/src/lib/nix/src

INC_DIR += $(NIX_DIR) $(REP_DIR)/include/nix

CC_WARN += -Wno-unused-function

CC_OPT += -DPACKAGE_VERSION=\"1.11\"

ifeq ($(findstring x86_64, $(SPECS)), x86_32)
ifeq ($(findstring nova, $(SPECS)), nova)
CC_OPT += -DSYSTEM=\"i686-nova-genode\"
endif
else
ifeq ($(findstring x86_64, $(SPECS)), x86_64)
ifeq ($(findstring nova, $(SPECS)), nova)
CC_OPT += -DSYSTEM=\"x86_64-nova-genode\"
else
CC_OPT += -DSYSTEM=\"unknown-unknown-genode\"
endif
else
CC_OPT += -DSYSTEM=\"unknown-unknown-genode\"
endif
endif
