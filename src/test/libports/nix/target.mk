TARGET = test-nix
LIBS = nixexpr nixformat nixmain nixstore nixutil
SRC_CC = main.cc

vpath main.cc $(PRG_DIR)/..
