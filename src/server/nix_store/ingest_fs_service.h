/*
 * \brief  Service for file system inputs and outputs
 * \author Emery Hemingway
 * \date   2015-06-27
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__INGEST_FS_SERVICE_H_
#define _NIX_STORE__INGEST_FS_SERVICE_H_

/* Genode includes */
#include <file_system/util.h>
#include <file_system_session/capability.h>

/* Nix includes */
#include <nix_store/derivation.h>

/* Local includes */
#include "ingest_component.h"

namespace Nix_store { class Ingest_service; }

class Nix_store::Ingest_service : public Genode::Service
{
	private:

		Server::Entrypoint             &_ep;
		Ingest_component                _component;
		File_system::Session_capability _cap = _ep.manage(_component);

		/**
		 * Create a link from input addressed path
		 * to ouput addressed path
		 */
		void _link_from_inputs(File_system::Session    &fs,
		                       char              const *id,
		                       char              const *path)
		{
			while (*path == '/') ++path;
			using namespace File_system;

			Nix_store::Name const name = _component.ingest(id);
			char const *final_str = name.string();

			/* create symlink at real file system */
			Dir_handle root = fs.dir("/", false);
			Handle_guard root_guard(fs, root);

			Symlink_handle link = fs.symlink(root, path, true);
			File_system::write(fs, link, final_str, Genode::strlen(final_str));
			fs.close(link);
		}

		/**
		 * Finalize the derivation outputs at the ingest session and
		 * create symlinks from the derivation outputs to hashed outputs.
		 */
		bool _finalize(File_system::Session &fs, Nix_store::Derivation &drv)
		{
			using namespace File_system;

			unsigned outstanding = 0;

			/* run thru the outputs and finalize the paths */
			drv.outputs([&] (Aterm::Parser &parser) {
				Nix_store::Name id;
				Nix_store::Name path;
				Nix_store::Name algo;
				Nix_store::Name hash;

				parser.string(&id);

				Nix_store::Name output = _component.ingest(id.string());
				if (output == "") {
					/* If output symlinks are missing, then failure is implicit. */
					PERR("'%s' not found at the ingest session", id.string());
					throw ~0;
				}

				parser.string(&path);
				parser.string(&algo);
				parser.string(&hash);

				/*
				if (((algo != "") || (hash != ""))
				 && !_local_session.verify(id.string(), algo.string(), hash.string()))
					throw Exception();
				*/
				++outstanding;
			});

			/*
			 * Run thru again and link the input paths to content paths.
			 * This happens in two steps because it is important than
			 * links are only created if all outputs are valid.
			 */
			drv.outputs([&] (Aterm::Parser &parser) {
				Nix_store::Name id;
				Nix_store::Name path;

				parser.string(&id);
				parser.string(&path);

				_link_from_inputs(fs, id.string(), path.string());
				--outstanding;

				parser.string(/* Algo */); 
				parser.string(/* Hash */);
			});
			return outstanding == 0;
		}

		void revoke_cap()
		{
			if (_cap.valid()) {
				_ep.dissolve(_component);
				_cap = File_system::Session_capability();
			}
		}

	public:

		/**
		 * Constructor
		 */
		Ingest_service(Nix_store::Derivation &drv, Server::Entrypoint &ep, Genode::Allocator &alloc)
		:
			Genode::Service("File_system"), _ep(ep), _component(ep, alloc)
		{ }

		~Ingest_service() { revoke_cap(); }

		bool finalize(File_system::Session &fs, Nix_store::Derivation &drv)
		{
			revoke_cap();

			try { return _finalize(fs, drv); }
			catch (...) { }
			return false;
		}


		/***********************
		 ** Service interface **
		 ***********************/

		Genode::Session_capability session(char const *args, Genode::Affinity const &) override
		{
			PDBG("%s", args);
			return _cap;
		}

		void upgrade(Genode::Session_capability, const char *args)
		{
			PERR("client is upgrading session, but don't know where to send it, %s", args);
			//Genode::env()->parent()->upgrade(_component.cap(), args);
		}

};

#endif
