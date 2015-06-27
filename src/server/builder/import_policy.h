/*
 * \brief  Policy and service for writing to the store
 * \author Emery Hemingway
 * \date   2015-06-27
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _BUILDER__IMPORT_POLICY_H_
#define _BUILDER__IMPORT_POLICY_H_

#include <store_import/session.h>
#include <file_system_session/capability.h>

namespace Builder { class Store_fs_policy; }

class Builder::Store_fs_policy
{
	private:

		Genode::Rpc_entrypoint         &_ep;
		Store_import::Session_component _local_session;
		File_system::Session_capability _local_session_cap;

		struct Local_service : public Genode::Service
		{
			Genode::Lock                     lock;
			File_system::Session_capability _cap;

			/**
			 * Constructor
			 */
			Local_service(File_system::Session_capability session_cap)
			: Genode::Service("File_system"), _cap(session_cap) { }

			Genode::Session_capability session(char const * /*args*/,
			                                   Genode::Affinity const &) {
				return _cap; }

			void upgrade(Genode::Session_capability, const char * /*args*/) { }
			void close(Genode::Session_capability) {
				lock.lock(); }

		} _local_service;

	public:

		/**
		 * Constructor
		 */
		Store_fs_policy(Server::Entrypoint &ep)
		:
			_ep(ep.rpc_ep()),
			_local_session(4096, 128*1024*2, ep),
			_local_session_cap(_ep.manage(&_local_session)),
			_local_service(_local_session_cap)
		{ }

		/**
		 * Destructor
		 */
		~Store_fs_policy() { _ep.dissolve(&_local_session); }

		/**
		 * Finalise the derivation outputs at the import session and
		 * create a symlinks to find them with.
		 *
		 * If an output path listed in a derivation exists in the store
		 * as a symlink, and the target of that symlink can be verified
		 * by replicating the hashing process over its contents, then
		 * that derivation output is verified.
		 */
		void finalize(File_system::Session &fs, Derivation &drv)
		{
			using namespace File_system;

			/* TODO
			 * The session probably closes on the same thread,
			 * but better safe than sorry.
			 */
			Lock::Guard guard(_local_service.lock);

			char name[MAX_NAME_LEN];
			char link[MAX_NAME_LEN];

			try {
				for (Derivation::Output *out = drv.output(); out; out = out->next()) {
					out->id.value(name, sizeof(name));

					_local_session.finish(name);
				}
			} catch (File_system::Lookup_failed) {
				/*
				 * If output symlinks are missing, then failure is implicit.
				 */
				return;
			}

			Dir_handle store_root = fs.dir("/", false);
			Handle_guard root_guard(fs, store_root);

			for (Derivation::Output *out = drv.output(); out; out = out->next()) {
				out->id.value(name, sizeof(name));
				char const *final = _local_session.final(name);
				out->path.value(link, sizeof(link));

				// TODO:
				// this is just a work around for an error in the Nix port
				char *link_name = link;

				for (char *p = link_name; *p;)
					if (*p++ == '/')
						link_name = p;

				try {
					Symlink_handle link = fs.symlink(store_root, link_name, true);
					Handle_guard link_guard(fs, link);
					write(fs, link, final, strlen(final));
				} catch (...) {
					/*
					 * A failure at this point does not
					 * effect the validity of outputs.
					 */
					PERR("error creating symlink %s to %s", link, final);
				}

			}
		}

		/***********************
		 ** Service interface **
		 ***********************/

		Genode::Service *service() { return &_local_service; }

		Genode::Service *resolve_session_request(const char *service_name,
		                                         const char *args)
		{
			if (!Genode::strcmp(service_name, "File_system"))
				return &_local_service;
			return 0;
		}

};

#endif