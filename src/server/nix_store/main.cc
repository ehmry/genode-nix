/*
 * \brief  Component for realizing Nix derivations
 * \author Emery Hemingway
 * \date   2015-03-13
 */

/* Genode includes */
#include <os/session_policy.h>
#include <os/server.h>
#include <root/component.h>
#include <base/service.h>
#include <util/arg_string.h>

/* Nix includes */
#include <nix_store_session/nix_store_session.h>

/* Local includes */
#include "ingest_component.h"
#include "build_component.h"

namespace Nix_store {

	class Ingest_root;
	class Build_root;
	class Main;

};


/**
 * File_system service for ingesting from naive clients
 */
class Nix_store::Ingest_root :
	public Genode::Root_component<Ingest_component>
{
	private:

		Server::Entrypoint &_ep;

	protected:

		Ingest_component *_create_session(const char *args)
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

			size_t session_size = sizeof(Ingest_component) + tx_buf_size;

			if (max((size_t)4096, session_size) > ram_quota) {
				Session_label const label(args);
				PERR("insufficient 'ram_quota' from %s, got %zd, need %zd",
				     label.string(), ram_quota, session_size);
				throw Root::Quota_exceeded();
			}
			ram_quota -= session_size;

			try {
				return new (md_alloc())
					Ingest_component(_ep, *md_alloc(), ram_quota, tx_buf_size);
			} catch (...) { PERR("cannot issue session"); }
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


class Nix_store::Build_root : public Genode::Root_component<Build_component>
{
	private:

		enum { MAX_LABEL_SIZE = 64 };

		Server::Entrypoint     &_ep;
		Genode::Allocator_avl   _fs_block_alloc;
		File_system::Connection _fs;
		Jobs                    _jobs;

	protected:

		Build_component *_create_session(const char *args, Affinity const &affinity) override
		{
			size_t ram_quota =
				Arg_string::find_arg(args, "ram_quota"  ).ulong_value(0);

			/*
			 * Check if donated ram quota suffices for session data,
			 * and communication buffer.
			 */
			size_t session_size = sizeof(Build_component);
			if (max((size_t)4096, session_size) > ram_quota) {
				PERR("insufficient 'ram_quota', got %zd, need %zd",
				     ram_quota, session_size);
				throw Root::Quota_exceeded();
			}

			return new(md_alloc())
				Build_component(md_alloc(), ram_quota, _fs, _jobs);
		}

	public:

		/**
		 * Constructor
		 */
		Build_root(Server::Entrypoint &ep,
		           Genode::Allocator &md_alloc)
		:
			Genode::Root_component<Build_component>(&ep.rpc_ep(), &md_alloc),
			_ep(ep),
			_fs_block_alloc(env()->heap()),
			_fs(_fs_block_alloc, 128*1024),
			_jobs(_ep, _fs)
		{
			/* look for dynamic linker */
			try {
				static Rom_connection rom("ld.lib.so");
				Process::dynamic_linker(rom.dataspace());
			} catch (...) { }

			using namespace File_system;
			static char const *placeholder = ".builder";

			/* verify permissions */
			try {
				Dir_handle root_handle = _fs.dir("/", false);
				Handle_guard root_guard(_fs, root_handle);

				try { _fs.unlink(root_handle, placeholder); }
				catch (Lookup_failed) { }

				_fs.close(_fs.file(
					root_handle, placeholder, READ_WRITE, true));
			} catch (...) {
				PERR("insufficient File_system access");
				throw;
			}
		}
};


struct Nix_store::Main
{
	Server::Entrypoint &ep;

	Sliced_heap sliced_heap = { env()->ram_session(), env()->rm_session() };

	Ingest_root ingest_root { ep, sliced_heap };
	Build_root   build_root { ep, sliced_heap };

	Main(Server::Entrypoint &ep)
	: ep(ep)
	{
		Genode::env()->parent()->announce(ep.manage(ingest_root));
		Genode::env()->parent()->announce(ep.manage(build_root));
	}
};


/************
 ** Server **
 ************/

namespace Server {

	char const *name() { return "nix_store_ep"; }

	size_t stack_size() { return 16*1024*sizeof(long); }

	void construct(Entrypoint &ep)
	{
		using namespace File_system;

		/* create an empty file to be sure we have write access */
		Genode::Allocator_avl   fs_alloc(env()->heap());
		File_system::Connection fs(fs_alloc, 4096);

		static char const *placeholder = ".nix_store";

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

		/* verify that ROM requests are routed properly */
		try { Rom_connection rom(placeholder); }
		catch (...) {
			PERR("failed to aquire store ROM");
			throw;
		}

		static Nix_store::Main inst(ep);
	}

}