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
		Session_component(Allocator            *session_alloc,
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
		 * Read a derivation and check that its inputs are valid.
		 */
		void check_inputs(char const *name)
		{
			Derivation(name).inputs([&] (Aterm::Parser &parser) {

				Genode::String<Builder::MAX_NAME_LEN> input;
				parser.string(&input);

				Derivation depend(input.string());

				parser.list([&] (Aterm::Parser &parser) {
					Genode::String<Builder::MAX_NAME_LEN> want_id;
					parser.string(&want_id);

					depend.outputs([&] (Aterm::Parser &parser) {
						/* XXX: figure out a limit for these output ids */
						Genode::String<96> id;
						parser.string(&id);

						if (id == want_id) {
							Genode::String<MAX_NAME_LEN> path;
							parser.string(&path);

							char const *output = path.string();
							/* XXX : slash hack */
							while (*output== '/') ++output;

							if (!valid(output)) {
								PERR("%s not valid", output);
								throw Missing_dependency();
							}
						} else {
							parser.string(); /* Path */
						}

						parser.string(); /* Algo */
						parser.string(); /* Hash */
					});
				});
			});
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

			/* XXX: slash hack */
			char const *name_str = name.string();
			while (*name_str == '/') ++name_str;

			path[0] = '/';
			strncpy(path+1, name_str, MAX_NAME_LEN);

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

			PLOG("realize %s", name);

			if (File_system::string_contains(name, '/')) {
				PERR("invalid derivation name %s", name);
				throw Invalid_derivation();
			}

			/* Prevent packet mixups. */
			collect_acknowledgements(*_store_fs.tx());

			/*
			 * Check that the derivation inputs are present,
			 * we don't take care of dependencies or scheduling,
			 * just keep a queue.
			 */
			try { check_inputs(name); }
			catch (Genode::Rom_connection::Rom_connection_failed) {
				PERR("Genode::Rom_connection::Rom_connection_failed");
				throw Invalid_derivation();
			}

			PLOG("queueing %s", name);
			_jobs.queue(name, sigh);
		}

};

#endif /* _BUILDER__STORE_SESSION_H_ */
