include $(call select_from_repositories,lib/mk/nix-common.inc)

INC_DIR      += $(NIX_DIR)/libstore $(REP_DIR)/src/lib/nixstore
REP_INC_DIR  += include/nix
