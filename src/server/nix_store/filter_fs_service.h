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

namespace Nix_store {

	class Filter_component;
	class Filter_service;

};


class Nix_store::Filter_component : public Genode::Rpc_object<File_system::Session, Filter_component>
{
	private:

		/**
		 * XXX: redundant with the inputs in Child
		 */
		struct Inputs : Genode::Avl_tree<Genode::Avl_string_base>
		{
			typedef Genode::Avl_string<Nix_store::MAX_NAME_LEN> Input;

			Genode::Allocator &alloc;

			Inputs(Derivation &drv, Genode::Allocator &alloc)
			: alloc(alloc)
			{
				/*
				 * XXX: move the symlink resolution to a lambda,
				 * that should make this easier to read
				 */

				Genode::Allocator_avl fs_tx_alloc(&alloc);
				File_system::Connection fs(fs_tx_alloc, 4096, "", "/", false);
				Dir_handle root_handle = fs.dir("/", false);

				/* read the derivation inputs */
				drv.inputs([&] (Aterm::Parser &parser) {

					Name input;
					parser.string(&input);

					/* load the input dependency */
					Derivation dependency(input.string());

					/* roll through the lists of inputs from this dependency */
					parser.list([&] (Aterm::Parser &parser) {

						Name want_id;
						parser.string(&want_id);

						/* roll through the dependency outputs to match the id */
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

								/* dereference the symlink */
								for (;;) try {
									if (input_path == "/" || input_path == "") {
										PERR("invalid derivation %s", input.string());
										throw Genode::Root::Unavailable();
									}
									Symlink_handle link = fs.symlink(root_handle, input_name, false);
									/* insert the symlink so the client can dereference it too */
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

								/* we now have a path that is not a symlink */
								insert(new (alloc) Input(input_name));

							} else {
								parser.string(); /* Path */
							}

							parser.string(); /* Algo */
							parser.string(); /* Hash */
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

		/**
		 * A File_system connection without a packet buffer
		 */
		struct Backend :
			Genode::Connection<File_system::Session>,
			Genode::Rpc_client<File_system::Session>
		{
			Backend()
			:
				Genode::Connection<File_system::Session>(
					session("ram_quota=%zd, tx_buf_size=%zd, writeable=0, label=\"filter\"",
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

			void sigh(Node_handle node, Genode::Signal_context_capability sigh) override { }

			void sync(Node_handle node) override
			{
				call<Rpc_sync>(node);
			}

		} _backend;

		Dir_handle _root_handle = _backend.dir("/", false);

	public:

		Filter_component(Derivation &drv, Genode::Allocator &alloc)
		: _inputs(drv, alloc)
		{ }


		/***************************
		 ** File_system interface **
		 ***************************/

		Genode::Capability<Tx> _tx_cap() { return _backend._tx_cap(); }

		Dir_handle dir(File_system::Path const &path, bool create) override
		{
			if (create) throw Permission_denied();
			if (!Genode::strcmp("/", path.string()))
				return _root_handle;
			_inputs.verify(path.string()+1);
			return _backend.dir(path, false);
		}

		File_handle file(Dir_handle dir_handle, File_system::Name const &name,
		                 Mode mode, bool create) override
		{
			if (create) throw Permission_denied();
			if (dir_handle == _root_handle)
				_inputs.verify(name.string());
			return _backend.file(dir_handle, name, mode, false);
		}

		Symlink_handle symlink(Dir_handle dir_handle, File_system::Name const &name, bool create) override
		{
			if (create) throw Permission_denied();
			if (dir_handle == _root_handle)
				_inputs.verify(name.string());
			return _backend.symlink(dir_handle, name, false);
		}

		Node_handle node(File_system::Path const &path) override
		{
			Nix_store::Path top_path(path.string());
			while(!top_path.has_single_element())
				top_path.strip_last_element();
			_inputs.verify(top_path.base()+1);
			return _backend.node(path);
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

		void sigh(Node_handle handle, Genode::Signal_context_capability sigh) override { }

		void sync(Node_handle handle) { _backend.sync(handle); }

		void control(Node_handle, Control) { }
};


/**
 * XXX: This class could be probably be merged with the Ingest_service
 */
class Nix_store::Filter_service : public Genode::Service
{
	private:

		Server::Entrypoint             &_ep;
		Filter_component                _component;
		File_system::Session_capability _cap = _ep.manage(_component);

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
		Filter_service(Nix_store::Derivation &drv, Server::Entrypoint &ep, Genode::Allocator &alloc)
		:
			Genode::Service("File_system"), _ep(ep), _component(drv, alloc)
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
