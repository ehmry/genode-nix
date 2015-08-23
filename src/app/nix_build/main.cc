/*
 * \brief  Utility to build Nix expressions
 * \author Emery Hemingway
 * \date   2015-08-15
 */

/* Genode includes */
#include <file_system/util.h>
#include <file_system_session/connection.h>

//#include <vfs/dir_file_system.h>
//#include <vfs/file_system_factory.h>
#include <os/config.h>
#include <base/signal.h>
#include <base/allocator_avl.h>

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

#include <iostream>
#include <map>
#include <string>


int main(void)
{
	nix::handleExceptions("nix_build", [&] {
		nix::initNix();
	});

	/* Use a File_system session for the sake of receiving edit signals. */
	Genode::Allocator_avl   tx_block_alloc(Genode::env()->heap());
	File_system::Connection fs(tx_block_alloc);

	File_system::File_handle default_handle;
	{
		File_system::Dir_handle root_handle = fs.dir("/", false);
		File_system::Handle_guard root_guard(fs, root_handle);
		try {
			default_handle = fs.file(root_handle, "default.nix",
			                         File_system::READ_ONLY, false);
		} catch (File_system::Lookup_failed) {
			PERR("lookup failed for 'default.nix'");
			return 1;
		}
	}
	File_system::Handle_guard default_guard(fs, default_handle);

	Genode::Signal_context  sig_ctx;
	Genode::Signal_receiver sig_rec;
	Genode::Signal_context_capability sig_cap = sig_rec.manage(&sig_ctx);

	nix::Strings searchPath;
	nix::EvalState state(searchPath);
	nix::Path exprPath("/default.nix");

	/*
	 * Nix may throw errors as exceptions
	  *to catch and interprete before exit.
	 */
	for (;;) {
		fs.sigh(default_handle, sig_cap);

		nix::handleExceptions("nix_build", [&] {
			using namespace nix;
			Value v, result;

			state.evalFile(exprPath, v);
			state.forceValue(v);
			Bindings & bindings(*state.allocBindings(0));
			state.autoCallFunction(bindings, v, result);
			state.forceValue(v);

			DrvInfo drvInfo(state);
			if (!getDerivation(state, v, drvInfo, false))
				throw Error("expression does not evaluation to a derivation, so I can't build it");
			Path drvPath = drvInfo.queryDrvPath();

			PathSet paths { drvPath };

			store()->buildPaths(paths, bmNormal);

			std::cout << result << std::endl;

			state.resetFileCache();
		});

		sig_rec.wait_for_signal();
	}
}