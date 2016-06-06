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

		/*
		struct Inputs : Genode::Avl_tree<Genode::Avl_string_base>
		{
			typedef Genode::Avl_string<Nix_store::MAX_NAME_LEN> Input;

			Genode::Allocator &alloc;

			Inputs(Genode::Env &env, Genode::Allocator &alloc, Derivation &drv)
			: alloc(alloc)
			{
				Genode::Allocator_avl fs_tx_alloc(&alloc);
				Nix::File_system_connection fs(env, fs_tx_alloc, "/", false, 4096);
				Dir_handle root_handle = fs.dir("/", false);

				drv.inputs([&] (Aterm::Parser &parser) {

					Name input;
					parser.string(&input);

					Derivation dependency(input.string());

					parser.list([&] (Aterm::Parser &parser) {

						Name want_id;
						parser.string(&want_id);

						dependency.outputs([&] (Aterm::Parser &parser) {

							Name id;
							parser.string(&id);

							if (id == want_id) {
								Nix_store::Path input_path;
								char *input_name = input_path.base()+1;

								{
									Name path;
									parser.string(&path);
									input_path.import(path.string(), "/");
								}

								for (;;) try {
									if (input_path == "/" || input_path == "") {
										PERR("invalid derivation %s", input.string());
										throw Genode::Root::Unavailable();
									}
									Symlink_handle link = fs.symlink(root_handle, input_name, false);
									insert(new (alloc) Input(input_name));
									size_t n = read(fs, link, input_name, input_path.capacity()-1, 0);
									input_name[n] = '\0';
									fs.close(link);
									while (!input_path.has_single_element())
										input_path.strip_last_element();
								} catch (Lookup_failed) {
									try { fs.close(fs.node(input_path.base())); break; }
									catch (Lookup_failed) {
										PERR("found dangling symlink to `%s'", input_path.base());
										throw Genode::Root::Unavailable();
									}
								} catch (...) {
									PERR("failed to access input `%s'", input_path.base());
									throw Genode::Root::Unavailable();
								}

								insert(new (alloc) Input(input_name));

							} else {
								parser.string();
							}

							parser.string();
							parser.string();
						});

					});

				});
			}

			~Inputs() { while (first()) destroy(alloc, first()); }

			void verify(char const *name) const
			{
				if (!(first() && first()->find_by_name(name)))
					throw Lookup_failed();
			}

		} const _inputs;
		*/

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
		: _inputs(inputs), _backend(env)
		{ PDBG(""); }

		Input const &_lookup_input(char const *name)
		{
			Input const *input = _inputs.lookup(name);
			if (!input) {
				PERR("lookup of %s at filter failed", name);
				throw Lookup_failed();
			}
			return *input;
		}

		void _resolve(Nix_store::Path &new_path, char const *orig)
		{
			Nix_store::Path old_path(orig);
			while(!old_path.has_single_element())
				old_path.strip_last_element();

			Input const &input = _lookup_input(old_path.base()+1);

			while (*orig && *orig != '/')
				++orig;

			new_path.import(input.final.string(), "/");
			new_path.append(orig);

			PDBG("%s -> %s", old_path.base(), new_path.base());
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
			if (!*path_str) throw Lookup_failed();
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
			PDBG("%s", args);
			return _cap;
		}

		void upgrade(Genode::Session_capability, const char *args)
		{
			PERR("client is upgrading session, but don't know where to send it, %s", args);
			//Genode::env()->parent()->upgrade(_nix_store.cap(), args);
		}

};


#endif
