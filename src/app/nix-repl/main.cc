/* Genode includes */
#include <builder_session/connection.h>
#include <terminal_session/connection.h>
#include <timer_session/connection.h>

/* Local includes */
#include "line_editor.h"
#include "format_util.h"
#include "nix-repl.h"

/* TODO: look this header over */
#include <nix/exceptions.h>


int main(void)
{
	using Genode::Signal_context;
	using Genode::Signal_context_capability;
	using Genode::Signal_receiver;

	static Terminal::Connection terminal;
	static Builder::Connection builder;

	static Signal_receiver sig_rec;
	static Signal_context  read_avail_sig_ctx;
	terminal.read_avail_sigh(sig_rec.manage(&read_avail_sig_ctx));
	//Jobs jobs(builder, sig_rec);

	enum { COMMAND_MAX_LEN = 1000 };
	static char buf[COMMAND_MAX_LEN];

	/*
	 * Nix may throw errors as exceptions to
	 * be caught and interpreted before exit.
	 */
	return handleExceptions("nix-repl", [] {

	nix::initNix();
	/*
	 * TODO: get search paths from config
	 *
	 * Seach paths can be referrenced in experessions with ' < ... >' brackets.
	 */
	nix::Strings searchPath;
	
	tprintf(terminal, "Welcome to Nix version " NIX_VERSION ". Type :? for help.\n\n");
	
	nix::NixRepl nix_repl(terminal, "nix_repl> ", buf, sizeof(buf), searchPath);

	for (;;) {
		/* block for event, e.g., the arrival of new user input */
		Genode::Signal signal = sig_rec.wait_for_signal();

		if (signal.context() == &read_avail_sig_ctx) {
			/* supply pending terminal input to line editor */
			while (terminal.avail() && !nix_repl.is_complete()) {
				char c;
				terminal.read(&c, 1);
				nix_repl.submit_input(c);
			}
		//} else if (char const *name = jobs.lookup_context(signal.context())) {
		//	tprintf(terminal, "\n%s exited\n", name);
		//	continue;
		}
		else {
			PERR("unknown signal context");
			continue;
		}

		if (!nix_repl.is_complete()) {
			continue;
		}

		nix_repl.evaluate();
		nix_repl.reset();
	}

	return 0;
	});
}