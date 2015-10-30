/* Genode includes */
#include <terminal_session/connection.h>
#include <timer_session/connection.h>

/* Local includes */
#include "line_editor.h"
#include "format_util.h"
#include "nix-repl.h"

/* Nix includes */
#include <shared.hh>


int main(void)
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

	/*
	 * Nix may throw errors as exceptions to
	 * be caught and interpreted before exit.
	 */
	return nix::handleExceptions("nix-repl", [] {
	
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