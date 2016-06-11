/*
 * \brief  Component for filtering access to the store
 * \author Emery Hemingway
 * \date   2016-04-16
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef __NIX_STORE__FILTER_FS_SERVICE_H_
#define __NIX_STORE__FILTER_FS_SERVICE_H_

/* Genode includes */
#include <file_system/util.h>
#include <file_system_session/connection.h>
#include <os/path.h>
#include <os/session_policy.h>
#include <os/server.h>
#include <base/allocator_guard.h>
#include <root/component.h>
#include <util/avl_tree.h>
#include <util/avl_string.h>

/* Nix includes */
#include <nix_store/derivation.h>

/* Local includes */
#include "environment.h"

namespace Nix_store {

	class Filter_component;
	class Filter_service;

};


class Nix_store::Filter_component : public Genode::Rpc_object<File_system::Session, Filter_component>
{
	private:

		Inputs const &_inputs;

		/**
		 * A File_system connection without a packet buffer
		 */
		struct Backend :
			Genode::Connection<File_system::Session>,
			Genode::Rpc_client<File_system::Session>
		{
			Backend(Genode::Env &env)
			:
				Genode::Connection<File_system::Session>(env,
					session(env.parent(),
					        "ram_quota=%zd, tx_buf_size=%zd, writeable=0, label=\"store -> filter\"",
					        8*1024*sizeof(long) + File_system::DEFAULT_TX_BUF_SIZE,
					        File_system::DEFAULT_TX_BUF_SIZE)),
				Rpc_client<File_system::Session>(cap())
			{ }

			Genode::Capability<Tx> _tx_cap() { return call<Rpc_tx_cap>(); }

			/***********************************
			 ** File-system session interface **
			 ***********************************/

			File_handle file(Dir_handle dir, File_system::Name const &name, Mode mode, bool) override 
			{
				return call<Rpc_file>(dir, name, mode, false);
			}

			Symlink_handle symlink(Dir_handle dir, File_system::Name const &name, bool) override
			{
				return call<Rpc_symlink>(dir, name, false);
			}

			Dir_handle dir(File_system::Path const &path, bool) override
			{
				return call<Rpc_dir>(path, false);
			}

			Node_handle node(File_system::Path const &path) override
			{
				return call<Rpc_node>(path);
			}

			void close(Node_handle node) override
			{
				call<Rpc_close>(node);
			}

			Status status(Node_handle node) override
			{
				return call<Rpc_status>(node);
			}

			void control(Node_handle node, Control control) override { }

			void unlink(Dir_handle dir, File_system::Name const &name) override { }

			void truncate(File_handle file, file_size_t size) override { }

			void move(Dir_handle from_dir, File_system::Name const &from_name,
			          Dir_handle to_dir,   File_system::Name const &to_name) override { }

			bool sigh(Node_handle node, Genode::Signal_context_capability sigh) override {
				return false; }

			void sync(Node_handle node) override
			{
				call<Rpc_sync>(node);
			}

		} _backend;

		Dir_handle _root_handle = _backend.dir("/", false);

	public:

		Filter_component(Genode::Env &env, Inputs const &inputs)
		: _inputs(inputs), _backend(env) { }

		Input const &_lookup_input(char const *name)
		{
			Input const *input = _inputs.lookup(name);
			if (!input)
				throw Lookup_failed();
			return *input;
		}

		void _resolve(Nix_store::Path &new_path, char const *orig)
		{
			Nix_store::Path old_path(orig);
			while(!old_path.has_single_element())
				old_path.strip_last_element();

			Input const &input = _lookup_input(old_path.base()+1);

			char const *subpath = orig+1;
			while (*subpath && *subpath != '/')
				++subpath;

			new_path.import(input.final.string(), "/");
			new_path.append(subpath);
		}

		/***************************
		 ** File_system interface **
		 ***************************/

		Genode::Capability<Tx> _tx_cap() { return _backend._tx_cap(); }

		Dir_handle dir(File_system::Path const &path, bool create) override
		{
			char const *path_str = path.string();
			if (!*path_str) throw Lookup_failed();
			if (create) throw Permission_denied();
			if (!strcmp("/", path_str))
				return _root_handle;
			{
				Nix_store::Path new_path;
				_resolve(new_path, path.string());
				return _backend.dir(new_path.base(), false);
			}
		}

		File_handle file(Dir_handle dir_handle, File_system::Name const &name,
		                 Mode mode, bool create) override
		{
			if (create) throw Permission_denied();
			if (dir_handle == _root_handle) {
				Input const &input = _lookup_input(name.string());
				return _backend.file(dir_handle, input.final.string(), mode, false);
			}
			return _backend.file(dir_handle, name, mode, false);
		}

		Symlink_handle symlink(Dir_handle dir_handle, File_system::Name const &name, bool create) override
		{
			if (create) throw Permission_denied();
			if (dir_handle == _root_handle)
				throw Lookup_failed(); /* keep it simple */
			return _backend.symlink(dir_handle, name, false);
		}

		Node_handle node(File_system::Path const &path) override
		{
			char const *path_str = path.string();

			if (!*path_str)
				throw Lookup_failed();
			if (!strcmp("/", path_str))
				return _root_handle;

			{
				Nix_store::Path new_path;
				_resolve(new_path, path.string());
				return _backend.node(new_path.base());
			}
		}

		void close(Node_handle handle) override {
			return _backend.close(handle);
		}

		Status status(Node_handle handle) {
			return _backend.status(handle); }

		void unlink(Dir_handle dir_handle, File_system::Name const &name) {
			throw Permission_denied(); }

		void truncate(File_handle file_handle, file_size_t size) {
			throw Permission_denied(); }

		void move(Dir_handle from_dir_handle, File_system::Name const &from_name,
		          Dir_handle to_dir_handle,   File_system::Name const &to_name) {
			throw Permission_denied(); }

		bool sigh(Node_handle handle, Genode::Signal_context_capability sigh) override {
			return false; }

		void sync(Node_handle handle) { _backend.sync(handle); }

		void control(Node_handle, Control) { }
};


/**
 * XXX: This class could be probably be merged with the Ingest_service
 */
class Nix_store::Filter_service : public Genode::Service
{
	private:

		Genode::Env                    &_env;
		Filter_component                _component;
		File_system::Session_capability _cap = _env.ep().manage(_component);

		void revoke_cap()
		{
			if (_cap.valid()) {
				_env.ep().dissolve(_component);
				_cap = File_system::Session_capability();
			}
		}

	public:

		/**
		 * Constructor
		 */
		Filter_service(Genode::Env &env, Inputs const &inputs)
		:
			Genode::Service("File_system"), _env(env), _component(env, inputs)
		{ }

		~Filter_service() { revoke_cap(); }


		/***********************
		 ** Service interface **
		 ***********************/

		Genode::Session_capability session(char const *args, Genode::Affinity const &) override
		{
			return _cap;
		}

		void upgrade(Genode::Session_capability, const char *args)
		{
			Genode::error("client is upgrading session, but don't know where to send it, ", args);
			//_env.parent().upgrade(_nix_store.cap(), args);
		}

};


#endif
