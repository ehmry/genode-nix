/*
 * \brief  Builder session component
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _NIX_STORE__BUILD_SESSION_H_
#define _NIX_STORE__BUILD_SESSION_H_

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
#include "build_job.h"

namespace Nix_store {

	using Nix_store::Derivation;

	class Build_component;

};

class Nix_store::Build_component : public Genode::Rpc_object<Nix_store::Session>
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
		Build_component(Allocator            *session_alloc,
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

				Name input;
				parser.string(&input);

				Derivation depend(input.string());

				parser.list([&] (Aterm::Parser &parser) {
					Name want_id;
					parser.string(&want_id);

					depend.outputs([&] (Aterm::Parser &parser) {
						/* XXX: figure out a limit for these output ids */
						Genode::String<96> id;
						parser.string(&id);

						if (id == want_id) {
							Name path;
							parser.string(&path);

							char const *output = path.string();
							/* XXX : slash hack */
							while (*output== '/') ++output;

							if (!valid(output)) {
								PERR("missing dependency %s", output);
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


		/*************************
		 ** Nix_store interface **
		 *************************/

		/**
		 * Return true if a store object at 'name' exists in the
		 * store file system and is a regular file, a directory,
		 * or a symlink to another valid object.
		 */
		bool valid(Name const &name)
		{
			using namespace File_system;

			char path[Nix_store::MAX_NAME_LEN+1];

			/* XXX: slash hack */
			char const *name_str = name.string();
			while (*name_str == '/') ++name_str;

			path[0] = '/';
			strncpy(path+1, name_str, Nix_store::MAX_NAME_LEN);

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
					if (strcmp(name.string(), path, Nix_store::MAX_NAME_LEN))
						return valid(path);
				}}
			} catch (Lookup_failed) { }
			return false;
		}

		void realize(Name const &drv_name, Genode::Signal_context_capability sigh)
		{
			using namespace File_system;
			char const *name = drv_name.string();

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
				PERR("failed to load %s by ROM", name);
				throw Invalid_derivation();
			}
			catch (...) {
				PERR("invalid derivation %s", name);
				throw Invalid_derivation();
			}

			_jobs.queue(name, sigh);
		}

		Name dereference(Name const &name) override
		{
			using namespace File_system;

			char const *name_str = name.string();

			Genode::Path<Nix_store::MAX_NAME_LEN+1> path(name.string());

			try {
				Node_handle node = _store_fs.node(path.base());
				Handle_guard node_guard(_store_fs, node);

				switch (_store_fs.status(node).mode) {
				case Status::MODE_FILE:
				case Status::MODE_DIRECTORY:
					return name_str;
				case Status::MODE_SYMLINK: {
					Symlink_handle link = _store_fs.symlink(
						_store_dir, path.base()+1, false);
					Handle_guard link_guard(_store_fs, link);
					size_t n = read(_store_fs, link, path.base(), path.capacity());
					path.base()[(n < path.capacity() ? n : path.capacity()-1)] = '\0';

					return path.base();
				}}
			} catch (Lookup_failed) { }
			return "";
		}

};

#endif
