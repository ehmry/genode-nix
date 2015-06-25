TARGET = nix-repl

LIBS += stdcxx nixexpr nixformat nixmain nixstore nixutil

SRC_CC = main.cc

INC_DIR += $(PRG_DIR) $(call select_from_repositories,src/app/cli_monitor)

# TODO: get this from the port recipe
CC_CXX_OPT += -DNIX_VERSION=\"1.8\"

