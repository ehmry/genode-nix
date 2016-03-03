include $(REP_DIR)/lib/import/import-libgit2.mk

LIBGIT2_SRC = $(LIBGIT2_DIR)/src
LIBGIT2_XDIFF_SRC = $(LIBGIT2_DIR)/src/xdiff
LIBGIT2_TRANSPORTS_SRC = $(LIBGIT2_DIR)/src/transports
HTTP_PARSER_SRC = $(LIBGIT2_DIR)/deps/http-parser


SRC_C += $(notdir $(wildcard $(LIBGIT2_SRC)/*.c))
SRC_C += $(notdir $(wildcard $(LIBGIT2_XDIFF_SRC)/*.c))
SRC_C += $(notdir $(wildcard $(LIBGIT2_TRANSPORTS_SRC)/*.c))
SRC_C += $(notdir $(wildcard $(HTTP_PARSER_SRC)/*.c))

SRC_C += hash_generic.c realpath.c

INC_DIR += $(REP_DIR)/src/lib/libgit2
INC_DIR += $(LIBGIT2_SRC)
INC_DIR += $(HTTP_PARSER_SRC)

CC_OPT += -DNO_MMAP

SHARED_LIB = yes
LIBS += libc zlib

vpath %.c $(LIBGIT2_SRC)
vpath %.c $(LIBGIT2_SRC)/hash
vpath %.c $(LIBGIT2_SRC)/unix
vpath %.c $(LIBGIT2_XDIFF_SRC)
vpath %.c $(LIBGIT2_TRANSPORTS_SRC)
vpath %.c $(HTTP_PARSER_SRC)

