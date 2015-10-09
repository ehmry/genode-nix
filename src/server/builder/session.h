/*
 * \brief  Builder session component
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _BUILDER__STORE_SESSION_H_
#define _BUILDER__STORE_SESSION_H_

/* Genode includes */
#include <file_system_session/connection.h>
#include <file_system/util.h>
#include <base/snprintf.h>
#include <base/affinity.h>
#include <base/allocator_guard.h>
#include <base/printf.h>
#include <base/service.h>
#include <base/signal.h>
#include <ram_session/client.h>
#include <root/component.h>
#include <os/config.h>
#include <os/server.h>
#include <dataspace/client.h>
#include <rom_session/connection.h>
#include <cap_session/cap_session.h>

/* Local includes */
#include "job.h"

namespace Builder { class Session_component; };

class Builder::Session_component : public Genode::Rpc_object<Session>
{
	private:

		Genode::Allocator_guard  _session_alloc;
		File_system::Session    &_store_fs;
		File_system::Dir_handle  _store_dir;
		Jobs                    &_jobs;

	public:

		/**
		 * Constructor
		 */
		Session_component(Server::Entrypoint   &ep,
		                  Allocator            *session_alloc,
		                  size_t                ram_quota,
		                  File_system::Session &fs,
		                  Jobs                 &jobs)
		:
			_session_alloc(session_alloc, ram_quota),
			_store_fs(fs),
			_store_dir(_store_fs.dir("/", false)),
			_jobs(jobs)
		{ }

		/**
		 * Read a derivation and check that given output id is valid.
		 *
		 * A ROM connection is made rather than a file system request
		 * because the client presumably has the same file as a ROM
		 * dataspace from the same server, so caching is possible.
		 * The build child will be requesting ROMs from the store,
		 * so this also ensures that ROM requests are properly routed.
		 */
		void check_output(char const *name, char const *id)
		{
			Genode::Rom_connection drv_rom(name, "store");
			Genode::Rom_dataspace_capability drv_ds = drv_rom.dataspace();
			if (!drv_ds.valid())
				throw Missing_dependency();

			char const *data = Genode::env()->rm_session()->attach(drv_ds);

			struct Done { };

			Aterm::Parser parser(data, Dataspace_client(drv_ds).size());

			try { parser.constructor("Derive", [&]
			{
				/*************
				 ** Outputs **
				 *************/
				parser.list([&]
				{
					parser.tuple([&]
					{
						char output_id[96];
						parser.string().value(output_id, sizeof(output_id));
						if (!strcmp(id, output_id, sizeof(output_id))) {
							char output_name[MAX_NAME_LEN];
							parser.string().value(output_name, sizeof(output_name));

							/* XXX : slash hack */
							size_t i = 0;
							while (output_name[i] == '/') ++i;

							if (!valid(output_name))
								throw Missing_dependency();

							parser.string(); /* Algo */
							parser.string(); /* Hash */
						}
					});
				});
				/* A hack to return without defining the entire term. */
				throw Done();
			}); } catch (Done) { }
		}

		/**
		 * Read a derivation and check that its inputs are valid.
		 */
		void check_inputs(char const *name)
		{
			Genode::Rom_connection drv_rom(name, "store");
			Genode::Rom_dataspace_capability drv_ds = drv_rom.dataspace();
			if (!drv_ds.valid())
				throw Missing_dependency();

			char const *data = Genode::env()->rm_session()->attach(drv_ds);

			Aterm::Parser parser(data, Dataspace_client(drv_ds).size());

			struct Done { };

			try { parser.constructor("Derive", [&]
			{
				/*************
				 ** Outputs **
				 *************/

				parser.list([&]
				{
					parser.tuple([&]
					{
						parser.string(); /* Id */
						parser.string(); /* Path */
						parser.string(); /* Algo */
						parser.string(); /* Hash */
					});
				});

				/************
				 ** Inputs **
				 ************/

				parser.list([&]
				{
					parser.tuple([&]
					{
						char input_drv[Builder::MAX_NAME_LEN];
						parser.string().value(input_drv, sizeof(input_drv));
						parser.list([&] {
							char id[96];
							parser.string().value(id, sizeof(id));

							/*
							 * TODO:
							 * Skip the leading '/', but why not
							 * ignore compatibility and drop the '/'?
							 */
							check_output(input_drv+1, id);
						});
					});
				});
				/* A hack to return without defining the entire term. */
				throw Done();
			}); } catch (Done) { }
		}

		/*******************************
		 ** Builder session interface **
		 *******************************/

		/**
		 * Return true if a store object at 'name' exists in the
		 * store file system and is a regular file, a directory,
		 * or a symlink to another valid object.
		 */
		bool valid(Name const &name)
		{
			using namespace File_system;

			char path[Builder::MAX_NAME_LEN+1];

			path[0] = '/';
			strncpy(path+1, name.string(), MAX_NAME_LEN);

			try {
				Node_handle node = _store_fs.node(path);
				Handle_guard node_guard(_store_fs, node);

				switch (_store_fs.status(node).mode) {
				case Status::MODE_FILE:
					return true;
				case Status::MODE_DIRECTORY:
					return true;
				case Status::MODE_SYMLINK: {
					Symlink_handle link = _store_fs.symlink(_store_dir, path+1, false);
					Handle_guard link_guard(_store_fs, link);
					path[read(_store_fs, link, path, sizeof(path))] = '\0';

					for (char *p = path+1; *p; ++p)
						if (*p == '/')
							return false;

					/* It would be embarassing to run in loop. */
					if (strcmp(name.string(), path, MAX_NAME_LEN))
						return valid(path);
				}}
			} catch (Lookup_failed) { }
			return false;
		}

		void realize(Name const &drv_name, Genode::Signal_context_capability sigh)
		{
			using namespace File_system;
			char const *name = drv_name.string();

			if (File_system::string_contains(name, '/'))
				throw Invalid_derivation();

			/* Prevent packet mixups. */
			collect_acknowledgements(*_store_fs.tx());

			/*
			 * Check that the derivation inputs are present,
			 * we don't take care of dependencies or scheduling,
			 * just keep a queue.
			 */
			check_inputs(name);

			_jobs.queue(name, sigh);
		}

};

#endif /* _BUILDER__STORE_SESSION_H_ */