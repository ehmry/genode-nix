/*
 * \brief  File system for ingesting to a store directory
 * \author Emery Hemingway
 * \date   2015-05-28
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <store_ingest_session/store_ingest_session.h>
#include <os/session_policy.h>
#include <os/server.h>
#include <root/component.h>

#include "fs_component.h"
#include "ingest_component.h"

namespace Store_ingest {
	class Ingest_root;
	class     Fs_root;
	struct Main;
}


class Store_ingest::Ingest_root :
	public Genode::Root_component<Ingest_component>
{
	private:

		Server::Entrypoint &_ep;

	protected:

		Ingest_component *_create_session(const char *args) override
		{
			size_t ram_quota =
				Arg_string::find_arg(args, "ram_quota").ulong_value(0);
			size_t tx_buf_size =
				Arg_string::find_arg(args, "tx_buf_size")
					.ulong_value(File_system::DEFAULT_TX_BUF_SIZE*2);
			if (!tx_buf_size)
				throw Root::Invalid_args();

			try {
				return new (md_alloc())
					Ingest_component(_ep, *md_alloc(), ram_quota, tx_buf_size);
			} catch (Genode::Allocator::Out_of_memory) {
				/* XXX: redundant, Root::Component should catch this */
				throw Root::Quota_exceeded();
			} catch (Genode::Parent::Service_denied) {
				PERR("cannot issue session, parent service denied");
			} catch (...) {
				PERR("cannot issue session");
			}
			throw Root::Unavailable();
		}

		void _upgrade_session(Ingest_component *session, const char *args) override
		{
			size_t ram_quota = Arg_string::find_arg(args, "ram_quota").ulong_value(0);
			session->upgrade_ram_quota(ram_quota);		
		}

	public:

		Ingest_root(Server::Entrypoint &ep, Allocator &alloc)
		:
			Genode::Root_component<Ingest_component>(&ep.rpc_ep(), &alloc),
			_ep(ep)
		{ }
};


class Store_ingest::Fs_root :
	public Genode::Root_component<Fs_component>
{
	private:

		Server::Entrypoint &_ep;

	protected:

		Fs_component *_create_session(const char *args)
		{
			if (!Arg_string::find_arg(args, "writeable").bool_value(true)) {
				PERR("refusing read-only session");
				throw Root::Invalid_args();
			}

			size_t tx_buf_size =
				Arg_string::find_arg(args, "tx_buf_size")
					.ulong_value(File_system::DEFAULT_TX_BUF_SIZE);
			if (!tx_buf_size)
				throw Root::Invalid_args();

			size_t ram_quota =
				Arg_string::find_arg(args, "ram_quota").ulong_value(0);

			size_t session_size = sizeof(Fs_component) + tx_buf_size;

			if (max((size_t)4096, session_size) > ram_quota) {
				Session_label const label(args);
				PERR("insufficient 'ram_quota' from %s, got %zd, need %zd",
				     label.string(), ram_quota, session_size);
				throw Root::Quota_exceeded();
			}
			ram_quota -= tx_buf_size;

			try {
				return new (md_alloc())
					Fs_component(_ep, *md_alloc(), ram_quota, tx_buf_size);
			} catch (...) { PERR("cannot issue session"); }
			throw Root::Unavailable();
		}

		void _upgrade_session(Fs_component *session, const char *args) override
		{
			size_t ram_quota = Arg_string::find_arg(args, "ram_quota").ulong_value(0);
			session->upgrade_ram_quota(ram_quota);		
		}

	public:

		Fs_root(Server::Entrypoint &ep, Allocator &alloc)
		:
			Genode::Root_component<Fs_component>(&ep.rpc_ep(), &alloc),
			_ep(ep)
		{ }
};


struct Store_ingest::Main
{
	Server::Entrypoint &ep;

	Sliced_heap sliced_heap = { env()->ram_session(), env()->rm_session() };

	Ingest_root ingest_root { ep, sliced_heap };
	Fs_root         fs_root { ep, sliced_heap };

	Main(Server::Entrypoint &ep)
	: ep(ep)
	{
		Genode::env()->parent()->announce(ep.manage(ingest_root));
		Genode::env()->parent()->announce(ep.manage(fs_root));
	}
};


/************
 ** Server **
 ************/

namespace Server {

	char const* name() { return "store_ingest_ep"; }

	size_t stack_size() { return 4*1024*sizeof(long); }

	void construct(Entrypoint &ep)
	{
		using namespace File_system;

		/* create an empty file to be sure we have write access */
		Genode::Allocator_avl   fs_alloc(env()->heap());
		File_system::Connection fs(fs_alloc, 4096);

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
			Genode::env()->parent()->exit(~0);
			throw;
		}

		static Store_ingest::Main inst(ep);
	}

}