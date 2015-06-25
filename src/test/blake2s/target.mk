TARGET   = test-blake2s
SRC_CC   = main.cc
LIBS     = base blake2s
INC_DIR += $(PRG_DIR)

vpath main.cc $(PRG_DIR)