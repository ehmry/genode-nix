SRC_CC = vfs.cc
LIBS   = nixmain nixexpr nixstore nixutil stdcxx
INC_DIR += $(REP_DIR)/src/lib/vfs/nix
vpath %.cc $(REP_DIR)/src/lib/vfs/nix

SHARED_LIB = yes
