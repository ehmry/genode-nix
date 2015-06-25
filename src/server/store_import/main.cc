/*
 * \brief  File system for importing to store
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
#include <root/component.h>
#include <base/allocator_avl.h>
#include <os/server.h>
#include <util/list.h>

/* Local includes */
#include "session.h"

namespace Store_import {
	class Root_component;
	struct Main;
}


class Store_import::Root_component : public Genode::Root_component<Session_component>
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

	public:

		Root_component(Server::Entrypoint &ep, Allocator &alloc)
		:
			Genode::Root_component<Session_component>(&ep.rpc_ep(), &alloc),
			_ep(ep)
		{ }
};


struct Store_import::Main
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

	char const* name() { return "store_import_ep"; }

	size_t stack_size() { return 4*1024*sizeof(long); }

	void construct(Entrypoint &ep) { static Store_import::Main inst(ep); }

}