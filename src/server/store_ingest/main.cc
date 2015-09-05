/*
 * \brief  File system for ingesting to a store directory
 * \author Emery Hemingway
 * \date   2015-05-28
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <store_ingest/session.h>
#include <root/component.h>
#include <base/allocator_avl.h>
#include <os/server.h>
#include <util/list.h>

namespace Store_ingest {
	class Root_component;
	struct Main;
}


class Store_ingest::Root_component : public Genode::Root_component<Session_component>
{
	private:

		Server::Entrypoint &_ep;

	protected:

		Session_component *_create_session(const char *args)
		{
			size_t ram_quota =
				Arg_string::find_arg(args, "ram_quota"  ).ulong_value(0);
			size_t tx_buf_size =
				Arg_string::find_arg(args, "tx_buf_size").ulong_value(0);

			/*
			 * Check if donated ram quota suffices for session data,
			 * and communication buffer.
			 */
			size_t session_size = sizeof(Session_component) + tx_buf_size;
			if (max((size_t)4096, session_size) > ram_quota) {
				PERR("insufficient 'ram_quota', got %zd, need %zd",
				     ram_quota, session_size);
				throw Root::Quota_exceeded();
			}

			try {
				return new (md_alloc()) Session_component(ram_quota, tx_buf_size, _ep);
			} catch (...) {
				throw Root::Unavailable();
			}
			return 0;
		}

		void _upgrade_session(Session_component *s, const char *args)
		{
			size_t ram_quota = Arg_string::find_arg(args, "ram_quota").ulong_value(0);
			s->upgrade_ram_quota(ram_quota);
		}

	public:

		Root_component(Server::Entrypoint &ep, Allocator &alloc)
		:
			Genode::Root_component<Session_component>(&ep.rpc_ep(), &alloc),
			_ep(ep)
		{
			/*
			 * create a placeholder file to be sure we have write access
			 */
			Genode::Allocator_avl   fs_alloc(env()->heap());
			File_system::Connection fs(fs_alloc, 32);

			static char const *placeholder = ".store";

			try {
				Dir_handle root_handle = fs.dir("/", false);
				Handle_guard guard(fs, root_handle);

				try { fs.unlink(root_handle, placeholder); }
				catch (Lookup_failed) { }

				fs.close(fs.file(
					root_handle, placeholder, READ_WRITE, true));
			} catch (...) {
				PERR("access issues at backend");
				throw;
			}
		}
};


struct Store_ingest::Main
{
	Server::Entrypoint &ep;

	Sliced_heap sliced_heap = { env()->ram_session(), env()->rm_session() };

	Root_component root { ep, sliced_heap };

	Main(Server::Entrypoint &ep)
	: ep(ep) {
		Genode::env()->parent()->announce(ep.manage(root)); }
};


/************
 ** Server **
 ************/

namespace Server {

	char const* name() { return "store_ingest_ep"; }

	size_t stack_size() { return 4*1024*sizeof(long); }

	void construct(Entrypoint &ep) { static Store_ingest::Main inst(ep); }

}