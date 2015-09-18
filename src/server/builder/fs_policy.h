/*
 * \brief  Policy and service for File_system access to the store
 * \author Emery Hemingway
 * \date   2015-06-27
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _BUILDER__FS_POLICY_H_
#define _BUILDER__FS_POLICY_H_

#include <store_ingest/session.h>
#include <file_system_session/capability.h>

namespace Builder { class Store_fs_policy; }

class Builder::Store_fs_policy
{
	private:

		Genode::Rpc_entrypoint          &_ep;
		Store_ingest::Session_component  _local_session;
		File_system::Session_capability  _local_session_cap;

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
		
		Parent_service &_parent_service;

	public:

		/**
		 * Constructor
		 */
		Store_fs_policy(Parent_service &fs_backend, Server::Entrypoint &ep)
		:
			_ep(ep.rpc_ep()),
			/* xxx: get this quota from the root child policy */
			_local_session(env()->ram_session()->quota() / 4, 128*1024*2, ep),
			_local_session_cap(_ep.manage(&_local_session)),
			_local_service(_local_session_cap),
			_parent_service(fs_backend)
		{ }

		/**
		 * Destructor
		 */
		~Store_fs_policy() { _ep.dissolve(&_local_session); }

		/**
		 * Finalise the derivation outputs at the ingest session and
		 * create symlinks from the derivation outputs to hashed outputs.
		 */
		bool finalize(File_system::Session &fs, Derivation &drv)
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
				PERR("%s not found at the ingest session", name);
				return false;
			}

			Dir_handle store_root = fs.dir("/", false);
			Handle_guard root_guard(fs, store_root);

			bool success = true;
			for (Derivation::Output *out = drv.output(); out; out = out->next()) {
				out->id.value(name, sizeof(name));
				char const *final = _local_session.final(name);
				out->path.value(link, sizeof(link));

				char *link_name = link;
				while (*link_name == '/')
					++link_name;

				size_t want_len = strlen(final)+1;
				size_t write_len = 0;
				try {
					Symlink_handle link = fs.symlink(store_root, link_name, true);
					Handle_guard link_guard(fs, link);
					write_len = write(fs, link, final, want_len);
				} catch (Node_already_exists) {
					PWRN("a symlink was already found at %s", link_name);
					Symlink_handle link = fs.symlink(store_root, link_name, false);
					Handle_guard link_guard(fs, link);
					write_len = write(fs, link, final, want_len);
				} catch (...) {
					/*
					 * A failure at this point does not
					 * effect the validity of other outputs.
					 */
					PERR("error creating symlink %s to %s", link_name, final);
				}
				success = want_len == write_len;
			}
			return success;
		}

		Genode::Service *service() { return &_local_service; }

};

#endif