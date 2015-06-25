include $(call select_from_repositories,lib/mk/nix-common.inc)

INC_DIR      += $(NIX_DIR)/libexpr
REP_INC_DIR  += include/nix

.PHONY: nix_corepkgs.tar

$(TARGET): nix_corepkgs.tar
nix_corepkgs.tar:
	$(VERBOSE) cd $(NIX_DIR)/../corepkgs; tar cf $(PWD)/bin/$@ .