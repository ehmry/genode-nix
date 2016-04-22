/* Genode includes */
#include <terminal_session/connection.h>
#include <timer_session/connection.h>

/* Local includes */
#include "line_editor.h"
#include "format_util.h"
#include "nix-repl.h"

/* Nix includes */
#include <shared.hh>


int _main(void)
{
	using Genode::Signal_context;
	using Genode::Signal_context_capability;
	using Genode::Signal_receiver;

	static Terminal::Connection terminal;

	static Signal_receiver sig_rec;
	static Signal_context  read_avail_sig_ctx;
	terminal.read_avail_sigh(sig_rec.manage(&read_avail_sig_ctx));

	enum { COMMAND_MAX_LEN = 1000 };
	static char buf[COMMAND_MAX_LEN];

	Vfs::Dir_file_system vfs(
		Genode::config()->xml_node().sub_node("vfs"),
		Vfs::global_file_system_factory());

	/*
	 * Nix may throw errors as exceptions to
	 * be caught and interpreted before exit.
	 */
	return nix::handleExceptions("nix-repl", [&] {

	nix::initNix(vfs);

	tprintf(terminal, "Welcome to Nix version " NIX_VERSION ". Type :? for help.\n\n");

	nix::NixRepl nix_repl(terminal, "nix-repl> ", buf, sizeof(buf));

	for (;;) {
		/* block for event, e.g., the arrival of new user input */
		sig_rec.wait_for_signal();

		/* supply pending terminal input to line editor */
		while (terminal.avail() && !nix_repl.is_complete()) {
			char c;
			terminal.read(&c, 1);
			nix_repl.submit_input(c);
		}

		if (!nix_repl.is_complete())
			continue;

		nix_repl.evaluate();
		nix_repl.reset();
	}

	return 0;
	});
}

typedef Genode::Thread<64*1024*sizeof(Genode::addr_t)> Big_thread_base;

struct Big_thread : public Big_thread_base
{
	int exit_code;

	Big_thread() : Big_thread_base("nix-repl") { };

	void entry() { exit_code = _main(); }
};

int main (void)
{
	static Big_thread thread;
	thread.start();
	thread.join();
	return thread.exit_code;
}
