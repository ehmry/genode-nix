/*
 * \brief  Builder server
 * \author Emery Hemingway
 * \date   2015-03-13
 */

/* Local includes */
#include "aterm_parser.h"
#include "config.h"
#include "session.h"

/* Genode includes */
#include <builder_session/builder_session.h>
#include <os/server.h>
#include <base/service.h>
#include <root/component.h>
#include <os/session_policy.h>
#include <util/arg_string.h>
#include <os/path.h>


namespace Builder {

	class Session_component;
	class Root_component;
	class Main;

	using namespace Genode;
};


class Builder::Root_component : public Genode::Root_component<Session_component>
{
	private:

		enum { MAX_LABEL_SIZE = 64 };

		Server::Entrypoint     &_ep;
		Ram_session_capability  _ram;
		Genode::Allocator_avl   _fs_block_alloc;
		File_system::Connection _fs;
		Job_queue               _job_queue;

	protected:

		Session_component *_create_session(const char *args, Affinity const &affinity) override
		{
			size_t ram_quota =
				Arg_string::find_arg(args, "ram_quota"  ).ulong_value(0);

			/*
			 * Check if donated ram quota suffices for session data,
			 * and communication buffer.
			 */
			size_t session_size = sizeof(Session_component);
			if (max((size_t)4096, session_size) > ram_quota) {
				PERR("insufficient 'ram_quota', got %zd, need %zd",
				     ram_quota, session_size);
				throw Root::Quota_exceeded();
			}

			return new(md_alloc())
				Session_component(fs_label(),
				                  _ep,
				                  md_alloc(), ram_quota,
				                  _fs,
				                  _job_queue);
		}

	public:

		/**
		 * Constructor
		 */
		Root_component(Server::Entrypoint &ep,
		               Genode::Allocator &md_alloc,
		               Ram_session_capability ram)
		:
			Genode::Root_component<Session_component>(&ep.rpc_ep(), &md_alloc),
			_ep(ep),
			_ram(ram),
			_fs_block_alloc(env()->heap()),
			_fs(_fs_block_alloc, 128*1024, fs_label()),
			_job_queue(_ep, _fs)
		{
			/* look for dynamic linker */
			try {
				static Rom_connection rom("ld.lib.so");
				Process::dynamic_linker(rom.dataspace());
			} catch (...) { }

			env()->parent()->announce(ep.manage(*this));
		}
};


struct Builder::Main
{
	Server::Entrypoint ep;

	Sliced_heap sliced_heap = { env()->ram_session(), env()->rm_session() };

	Root_component root = { ep, sliced_heap, env()->ram_session_cap() };

	Main(Server::Entrypoint &ep) : ep(ep) { }
};


/************
 ** Server **
 ************/

namespace Server {

	char const *name() { return "builder_ep"; }

	size_t stack_size() { return 8*1024*sizeof(long); }

	void construct(Entrypoint &ep)
	{
		static Builder::Main builder(ep);
	}

}