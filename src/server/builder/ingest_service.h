/*
 * \brief  Service for writing outputs to the store
 * \author Emery Hemingway
 * \date   2015-06-27
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _BUILDER__INGEST_SERVICE_H_
#define _BUILDER__INGEST_SERVICE_H_

#include <store_ingest_session/connection.h>
#include <file_system/util.h>
#include <file_system_session/capability.h>

namespace Builder { class Store_ingest_service; }

class Builder::Store_ingest_service : public Genode::Service
{
	private:

		Store_ingest::Connection _ingest;

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

			Store_ingest::Name const name = _ingest.ingest(id);
			char const *final_str = name.string();

			/* create symlink at real file system */
			Dir_handle root = fs.dir("/", false);
			Handle_guard root_guard(fs, root);

			Symlink_handle link =
				fs.symlink(root, path, true);
			Handle_guard link_guard(fs, link);

			File_system::write(fs, link, final_str, strlen(final_str));
		}

		/**
		 * Finalize the derivation outputs at the ingest session and
		 * create symlinks from the derivation outputs to hashed outputs.
		 */
		bool _finalize(File_system::Session &fs, Derivation &drv)
		{
			using namespace File_system;

			unsigned outstanding = 0;

			/* run thru the outputs and finalize the paths */
			drv.outputs([&] (Aterm::Parser &parser) {
				String<MAX_NAME_LEN> id;
				String<MAX_NAME_LEN> path;
				String<MAX_NAME_LEN> algo;
				String<MAX_NAME_LEN> hash;

				parser.string(&id);

				Store_ingest::Name output = _ingest.ingest(id.string());
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
				String<MAX_NAME_LEN> id;
				String<1024> path;

				parser.string(&id);
				parser.string(&path);

				try {
					_link_from_inputs(fs, id.string(), path.string());
					--outstanding;
				} catch (...) {
					PERR("failed to create link at %s", path.string());
				}

				parser.string(/* Algo */); 
				parser.string(/* Hash */);
			});
			return outstanding == 0;
		}

	public:

		/**
		 * Constructor
		 */
		Store_ingest_service(Derivation &drv)
		: Genode::Service("File_system")
		{
			/* declare the outputs in advance */
			drv.outputs([&] (Aterm::Parser &parser) {
				Store_ingest::Name id;

				parser.string(&id); _ingest.expect(id);
				parser.string(/* path */);
				parser.string(/* algo */);
				parser.string(/* hash */);
			});
		}

		bool finalize(File_system::Session &fs, Derivation &drv)
		{
			_ingest.revoke_session();

			try { return _finalize(fs, drv); }
			catch (...) { }
			return false;
		}


		/***********************
		 ** Service interface **
		 ***********************/

		Session_capability session(char const *args, Affinity const &) override
		{
			PWRN("requesting fs session from ingest, %s", args);
			return _ingest.file_system_session(args);
		}

		void upgrade(Session_capability, const char *args)
		{
			PWRN("client is upgrading ingest session, %s", args);
			Genode::env()->parent()->upgrade(_ingest.cap(), args);
		}

};

#endif