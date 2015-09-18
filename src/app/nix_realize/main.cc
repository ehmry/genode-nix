/*
 * \brief  Utility to build Nix expressions
 * \author Emery Hemingway
 * \date   2015-08-29
 */

/* Nix includes */
#include <shared.hh>
#include <eval.hh>
#include <eval-inline.hh>
#include <store-api.hh>
#include <common-opts.hh>
#include <get-drvs.hh>
#include <derivations.hh>
#include <affinity.hh>
#include <attr-path.hh>

/* Stdcxx includes */
#include <iostream>
#include <map>
#include <string>

/* Genode includes */
#include <vfs/file_system_factory.h>
#include <vfs/dir_file_system.h>
#include <os/config.h>
#include <base/printf.h>


void eval_path(nix::EvalState &state, char const *path, nix::PathSet &drv_paths)
{
	using namespace nix;

	Value v;

	/* parse the expression and evaluate */
	state.evalFile(path, v);

	/* unthunk the evaluation */
	state.forceValue(v);

	DrvInfo drvInfo(state);
	if (!getDerivation(state, v, drvInfo, false)) {
		PERR("no derivation produced from %s", path);
		return;
	}

	Path drv_path = drvInfo.queryDrvPath();

	drv_paths.insert(drv_path);
}


int main(void)
{
	nix::handleExceptions("nix_realize", [&] {
	using namespace nix;

	Genode::Xml_node config_node;
	Genode::Xml_node vfs_node;
	try {
		config_node = Genode::config()->xml_node().sub_node("nix");
		vfs_node = config_node.sub_node("vfs");
	} catch (...) {
		PERR("failed to get nix configuration");
		throw;
	}

	Vfs::Dir_file_system vfs(
		vfs_node, Vfs::global_file_system_factory());

	nix::Vfs_root  vfs_root(vfs);
	nix::Store     store(vfs_root);
	nix::EvalState state(vfs_root, store, config_node);

	PathSet drv_paths;

	int files_evaulated = 0;
	config_node.for_each_sub_node("file", [&] (Genode::Xml_node file_node) {
		char path[Vfs::MAX_PATH_LEN];
		try {
			file_node.attribute("path").value(path, sizeof(path));
		} catch (Genode::Xml_node::Nonexistent_attribute) {
			PERR("'path' attribute mising from file node");
			return;
		}

		eval_path(state, path, drv_paths);
	});

	if (!files_evaulated)
		eval_path(state, "/default.nix", drv_paths);

	store.buildPaths(drv_paths, bmNormal);
}); }