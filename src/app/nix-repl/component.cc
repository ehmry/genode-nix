/* Genode includes */
#include <terminal_session/connection.h>
#include <timer_session/connection.h>
#include <base/attached_rom_dataspace.h>
#include <base/heap.h>
#include <base/component.h>

/* Local includes */
#include "line_editor.h"
#include "format_util.h"
#include "nix-repl.h"

/* Nix includes */
#include <shared.hh>


struct Main
{
	enum { COMMAND_MAX_LEN = 1024 };
	char buf[COMMAND_MAX_LEN];

	Genode::Env       &env;

	Terminal::Connection terminal { env };

	nix::NixRepl nix_repl;

	void read_terminal()
	{
		while (terminal.avail() && !nix_repl.is_complete()) {
			char c;
			terminal.read(&c, 1);
			nix_repl.submit_input(c);
		}

		if (nix_repl.is_complete())
			/*
			 * Nix may throw errors as exceptions to
			 * be caught and interpreted before exit.
			 */
			nix::handleExceptions("nix-repl", [&] {
				nix_repl.evaluate();
				nix_repl.reset();
			});
	}

	Genode::Signal_handler<Main> read_handler
		{ env.ep(), *this, &Main::read_terminal };

	Main(Genode::Env &env,
	     Genode::Allocator &alloc,
	     Genode::Xml_node config)
	:
		env(env),
		nix_repl(env, alloc, terminal, "nix-repl> ",
		         buf, sizeof(buf), config.sub_node("nix"))
	{
		terminal.read_avail_sigh(read_handler);
		tprintf(terminal, "Welcome to Nix version " NIX_VERSION ". Type :? for help.\n\n");
		nix_repl.reset();
	}

};

namespace Component {

	/* this is fuctional stuff, so use a huge stack */
	size_t stack_size() { return 64*1024*sizeof(Genode::addr_t); }

	void construct(Genode::Env &env)
	{
		static Genode::Attached_rom_dataspace config { env, "config" };
		static Genode::Heap heap { env.ram(), env.rm() };

		static Vfs::Dir_file_system vfs
			{ env, heap, config.xml().sub_node("vfs"),
			  Vfs::global_file_system_factory()
			};

		nix::initNix(vfs);

		static Main main(env, heap, config.xml());
	}

}
