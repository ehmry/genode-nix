/*
 * \brief  File system hashing proxy
 * \author Emery Hemingway
 * \date   2015-05-28
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__INGEST_COMPONENT_H_
#define _NIX_STORE__INGEST_COMPONENT_H_

/* Genode includes */
#include <store_hash/encode.h>
#include <file_system/util.h>
#include <file_system_session/connection.h>
#include <file_system_session/rpc_object.h>
#include <os/path.h>
#include <os/signal_rpc_dispatcher.h>
#include <os/server.h>
#include <util/string.h>
#include <base/allocator_avl.h>
#include <base/allocator_guard.h>

/* Local includes */
#include "ingest_node.h"

namespace Nix_store { class Ingest_component; }


class Nix_store::Ingest_component : public File_system::Session_rpc_object
{
	private:

		Genode::Allocator_guard _alloc;

		/* top level hash nodes */
		Hash_root_registry _root_registry { _alloc };

		/**
		 * This registry maps node handles from the backend
		 * store to the local tree of hashing nodes.
		 */
		Hash_node_registry _node_registry;

		/* a queue of packets from the client awaiting backend processing */
		File_system::Packet_descriptor      _packet_queue[TX_QUEUE_SIZE];

		Genode::Allocator_avl               _fs_tx_alloc { &_alloc };
		File_system::Connection_base        _fs;
		Signal_rpc_member<Ingest_component> _process_packet_dispatcher;
		Dir_handle                          _root_handle;
		bool                                _strict = false;


		/******************************
		 ** Packet-stream processing **
		 ******************************/

		/**
		 * Process and incoming client packet
		 *
		 * Return true if a response is needed from the backend, otherwise
		 * the packet may be immediately acknowledged as failed.
		 */
		bool _process_incoming_packet(File_system::Packet_descriptor &theirs)
		{
			void *content = tx_sink()->packet_content(theirs);
			size_t const length = theirs.length();

			if (!content || (length > theirs.size()) || (length == 0)
			    /* Reading the entries of the root is not allowed. */
			 || !theirs.handle().valid() || theirs.handle() == _root_handle)
				return false;

			try {
				/* Emulate the read of a symlink */
				if (theirs.handle().value & ROOT_HANDLE_PREFIX) {
					Hash_root &root = _root_registry.lookup(theirs.handle());
					if (root.done && theirs.operation() == File_system::Packet_descriptor::READ) {
						size_t name_len = strlen(root.filename);
						if (name_len <= length) {
							memcpy(content, root.filename, name_len);
							theirs.length(name_len);
						}
					}
					/* we read from a local virtual node, so ack immediately */
					return false;
				}

				/*
				 * Allocate a second packet from the backend
				 * and copy over the metadata.
				 */
				File_system::Session::Tx::Source &source = *_fs.tx();
				File_system::Packet_descriptor::Opcode op = theirs.operation();
				File_system::Packet_descriptor
					ours(source.alloc_packet(length),
					     theirs.handle(),
					     op,
					     length,
					     theirs.position());

				if (op == File_system::Packet_descriptor::WRITE) {
					Hash_node *hash_node = _node_registry.lookup(ours.handle());
					if (!hash_node) {
						/* if we don't hash it, they don't write it */
						PERR("no hash node found for handle on client packet");
						return false;
					}
					memcpy(source.packet_content(ours), content, length);
				}
				source.submit_packet(ours);
				return true;
			}
			catch (Invalid_handle) { PERR("Invalid_handle"); }
			catch (Lookup_failed)  { PERR("Lookup_failed"); }
			return false;
		}

		/**
		 * Read responses from the backend, return true if a client packet was matched
		 *
		 * We only hash packet conent after is acknowledged by the backend.
		 * We do not trust our client not to change the content of its shared
		 * packet buffer, but we have no choice but to trust the storage backend.
		 */
		bool _process_outgoing_packet(int const queue_size)
		{
			using namespace File_system;

			File_system::Session::Tx::Source &source = *_fs.tx();
			File_system::Packet_descriptor ours = source.get_acked_packet();

			int i = 0;
			for (; i < queue_size; ++i) {
				if (
				    (_packet_queue[i].handle()    == ours.handle()) &&
				    (_packet_queue[i].operation() == ours.operation()) &&
					(_packet_queue[i].position()  == ours.position())) {
					break;
				}
			}
			if (i == queue_size) {
				/* this is bad, now there is probably a stuck packet */
				PERR("unknown packet received from the backend");
				source.release_packet(ours);
				return false;
			}

			File_system::Packet_descriptor &theirs = _packet_queue[i];

			size_t length = ours.length();
			uint8_t const *content = (uint8_t const *)source.packet_content(ours);
			if (!content) {
				tx_sink()->acknowledge_packet(theirs);
				source.release_packet(ours);
				/* invalidate the packet in the queue */
				_packet_queue[i] = File_system::Packet_descriptor();
				return true;
			}

			switch (ours.operation()) {
				case File_system::Packet_descriptor::WRITE:
					try {
						Hash_node *hash_node = _node_registry.lookup(ours.handle());
						if (!hash_node) {
							length = 0;
							break;
						}

						hash_node->write(content, length, ours.position());
						break;
					} catch (Invalid_handle) {
						PERR("Invalid_handle");
						length = 0;
					}
					break;
				case File_system::Packet_descriptor::READ:
					void *dst = tx_sink()->packet_content(theirs);
					if (!dst)
						length = 0;

					memcpy(dst, content, length);
					break;
			}
			theirs.length(length);

			tx_sink()->acknowledge_packet(theirs);
			source.release_packet(ours);
			/* invalidate the packet in the queue */
			_packet_queue[i] = File_system::Packet_descriptor();
			return true;
		}

		/**
		 * There is as fair amount of logic here to deal with multiple incoming
		 * packets from the client, but in practice there is always just
		 * one. Nix is purely functional so in theory we can process elements
		 * of the concrete syntax tree and import objects in parallel, but for
		 * now its one thread and one packet at a time. The rest is for style.
		 */
		void _process_packets(unsigned)
		{
			/*
			 * Keep local copies of the client packets that come in
			 * and then send our own packets to the backend.
			 */
			int n = 0;
			while (tx_sink()->ready_to_ack()
			    && tx_sink()->packet_avail()
			    && n < TX_QUEUE_SIZE)
			{
				_packet_queue[n] = tx_sink()->get_packet();
				if (_process_incoming_packet(_packet_queue[n]))
					++n;
				else /* no action required at backend */
					tx_sink()->acknowledge_packet(_packet_queue[n]);
			}

			/*
			 * Block while waiting on the server.
			 */
			int outstanding = n;
			while (n)
				if (_process_outgoing_packet(n--))
					--outstanding;

			/*
			 * Acknowledge client packets that could not be matched
			 * to backend packets, this should never happen.
			 */
			while (outstanding--)
				for (int i = 0; i < TX_QUEUE_SIZE; ++i)
					if (_packet_queue[i].handle().valid()) {
						tx_sink()->acknowledge_packet(_packet_queue[i]);
						_packet_queue[i] = File_system::Packet_descriptor();
						break;
					}
		}

		File &_create_file_node(Dir_handle dir_handle, char const *name)
		{
			/* create the local node */
			if (dir_handle == _root_handle) {
				Hash_root &root = _root_registry.alloc_file(name, _strict);
				return *dynamic_cast<File *>(root.node);
			}

			Directory &dir_node = _node_registry.lookup_dir(dir_handle);
			return dir_node.file(name, true);
		}

		static bool is_root(File_system::Path const &path) {
			return Genode::strcmp(path.string(), "/", 2) == 0; }

		static void empty_dir(File_system::Session &fs, char const *path)
		{
			using namespace File_system;
			Directory_entry dirent;

			Dir_handle dir_handle = fs.dir(path, false);
			Handle_guard guard(fs, dir_handle);

			while (read(fs, dir_handle, &dirent, sizeof(dirent)) == sizeof(dirent)) {
				try {
					fs.unlink(dir_handle, dirent.name);
				} catch (Not_empty) {
					Genode::Path<MAX_PATH_LEN> subdir(dirent.name, path);
					empty_dir(fs, subdir.base());
					fs.unlink(dir_handle, dirent.name);
				}
			}
		}

	public:

		/**
		 * Constructor
		 *
		 * The tx buffer size is split between the local
		 * stream buffer and the backend buffer.
		 */
		Ingest_component(Server::Entrypoint &ep,
		                 Genode::Allocator  &alloc,
		                 size_t ram_quota = 16*4096,
		                 size_t tx_buf_size = File_system::DEFAULT_TX_BUF_SIZE*2)
		:
			Session_rpc_object(env()->ram_session()->alloc(tx_buf_size/2), ep.rpc_ep()),
			_alloc(&alloc, ram_quota),
			_fs(_fs_tx_alloc, tx_buf_size/2, "ingest"),
			_process_packet_dispatcher(ep, *this, &Ingest_component::_process_packets)
		{
			PDBG("");
			_root_handle = _fs.dir("/", false);

			/* invalidate the packet queue */
			for (int i = 0; i < TX_QUEUE_SIZE; ++i)
				_packet_queue[i] = File_system::Packet_descriptor();

			/*
			 * Register '_process_packets' dispatch function as signal
			 * handler for client packet submission signals.
			 */
			_tx.sigh_packet_avail(_process_packet_dispatcher);
			PDBG("done");
		}

		/**
		 * Destructor
		 */
		~Ingest_component()
		{
			Dataspace_capability ds = tx_sink()->dataspace();
			env()->ram_session()->free(static_cap_cast<Ram_dataspace>(ds));

			_root_registry.unlink(_fs, _root_handle);
		}

		void upgrade_ram_quota(size_t ram_quota) { _alloc.upgrade(ram_quota); }

		/**
		 * Used by the ingest component to restrict the root nodes
		 */
		void expect(char const *id)
		{
			_root_registry.alloc_root(id);
			_strict = true;
		}

		void finish(Hash_root &root)
		{
			if (root.done) return;

			_node_registry.close_all(_fs);

			/* Flush the root. */
			File *file_node = dynamic_cast<File *>(root.node);
			if (file_node) {
				File_handle final_handle = _fs.file(_root_handle, root.filename, READ_ONLY, false);
				Handle_guard guard(_fs, final_handle);
				file_node->flush(_fs, final_handle);
			} else {
				Directory *dir_node = dynamic_cast<Directory *>(root.node);
				if (!dir_node)
					throw Invalid_handle();

				char path[MAX_NAME_LEN+1];
				*path = '/';
				strncpy(path+1, root.filename, sizeof(path)-1);

				dir_node->flush(_fs, path);
			}

			uint8_t final_name[MAX_NAME_LEN];
			root.node->digest(&final_name[1], sizeof(final_name)-1);
			Store_hash::encode(&final_name[1], root.name, sizeof(final_name)-1);
			final_name[0] = '/';

			try {
				/* if the final path already exists, delete this ingest */
				_fs.close(_fs.node((char *)final_name));
				try {
					_fs.unlink(_root_handle, root.filename);
				} catch (Not_empty) {
					Genode::Path<MAX_PATH_LEN> path(root.filename);
					empty_dir(_fs, path.base());
					_fs.unlink(_root_handle, root.filename);
				}
			} catch (Lookup_failed) {
				/* move the ingest root to its final location */
				_fs.move(_root_handle, root.filename,
				         _root_handle, (char *)final_name+1);

				root.finalize((char *)final_name+1);
			}
		}

		/**
		 * Used by the ingest component to get the final name
		 *
		 * \throw Lookup_failed
		 */
		char const *ingest(char const *name)
		{
			try {
				Hash_root &root = _root_registry.lookup(name);
				finish(root);
				return root.filename;
			} catch (...) { }
			return 0;
		}

		/*
		bool verify(char const *name, char const *algo, char const *hash)
		{
			Hash_root &root = _root_registry.lookup(name);
			if (!root.done)
				throw Lookup_failed();

			if (!strcmp("blake2s", algo)) {
				uint8_t buf[53];
				root.node->digest(buf, sizeof(buf));
				Store_hash::encode_hash(buf, sizeof(buf));

				if (strcmp(hash, (char *)&buf, sizeof(buf))) {
					PERR("%s:%s:%s != %s", name, algo, hash, (char*)&buf);
					return false;
				}
				PLOG("verified %s:%s:%s", name, algo, (char*)&buf);
			}
			PERR("unhandled hash algorithm '%s'", algo);
			throw File_system::Exception();
		}
		*/


		/***************************
		 ** File_system interface **
		 ***************************/

		Dir_handle dir(File_system::Path const &path, bool create) override
		{
			if (is_root(path)) {
				if (create) throw Node_already_exists();
				else return _root_handle;
			}

			char new_path[MAX_PATH_LEN];
			char const *sub_path = split_path(new_path, path.string());
			char const *root_name = new_path+1;

			/* get a local node */
			Hash_root &root = (create && !*sub_path)
				? _root_registry.alloc_dir(root_name, _strict)
				: _root_registry.lookup(root_name);

			Directory *parent_dir = dynamic_cast<Directory *>(root.node);
			if (!parent_dir) {
				PERR("%s is not a directory", root_name);
				throw Lookup_failed();
			}

			Directory &dir_node = *sub_path ?
				parent_dir->dir(sub_path, create) : *parent_dir;

			/* get a handle for the remote directory */

			/* Rewrite the directory path. */
			strncpy(new_path+1, root.filename, sizeof(new_path)-1);
			if (*sub_path) {
				size_t len = strlen(new_path);
				new_path[len++] = '/';
				strncpy(new_path+len, sub_path, sizeof(new_path)-len);
			}

			Dir_handle handle;
			try {
				handle = _fs.dir(new_path, create);
			} catch (Permission_denied) {
				PERR("permission denied at backend"); throw;
			} catch (Out_of_metadata) {
				if ((_alloc.quota() - _alloc.consumed()) > 8192) {
					_alloc.withdraw(8192);
					Genode::env()->parent()->upgrade(_fs.cap(), "ram_quota=8K");
					handle = _fs.dir(new_path, create);
				} else
					throw;
			} /* TODO: destroy the node if creating */

			_node_registry.insert(handle, &dir_node);
			return handle;
		}

		File_handle file(Dir_handle dir_handle, File_system::Name const &name, Mode mode, bool create) override
		{
			File_handle handle;

			/* get a local node */
			Hash_root *root = nullptr;
			File *file_node;
			
			if (dir_handle == _root_handle) {
				if (create) {
					root = &_root_registry.alloc_file(name.string(), _strict);
				} else {
					root = &_root_registry.lookup(name.string());
				}
				if (!root)
					throw Lookup_failed();

				file_node = dynamic_cast<File *>(root->node);
				if (!file_node) {
					if (create)
						throw Node_already_exists();
					else
						throw Lookup_failed();
				}

			} else {
				Directory &dir_node = _node_registry.lookup_dir(dir_handle);
				file_node = &dir_node.file(name.string(), create);
			}

			/* get a handle for the remote file */
			try {
				handle = root
					? _fs.file(dir_handle, root->filename, mode, create)
					: _fs.file(dir_handle, name, mode, create);
			} catch (Permission_denied) {
				PERR("permission denied at backend"); throw;
			} catch (Out_of_metadata) {
				if ((_alloc.quota() - _alloc.consumed()) > 8192) {
					_alloc.withdraw(8192);
					Genode::env()->parent()->upgrade(_fs.cap(), "ram_quota=8K");
					handle = root
						? _fs.file(dir_handle, root->filename, mode, create)
						: _fs.file(dir_handle, name, mode, create);
					} else
						throw;
			} /* TODO: destroy the node if creating */

			/*
			 * If this node can't be used to modify data,
			 * then it is not a node we are concerned with.
			 */
			if (mode >= WRITE_ONLY)
				_node_registry.insert(handle, file_node);
			return handle;
		}

		Symlink_handle symlink(Dir_handle dir_handle, File_system::Name const &name, bool create) override
		{
			char const *name_str = name.string();

			if (dir_handle != _root_handle) {

				if (create &&
				    (_alloc.quota()-_alloc.consumed() < sizeof(File)))
					throw No_space();

				Symlink_handle handle;
				try {
					handle = _fs.symlink(dir_handle, name, create);
				} catch (Permission_denied) {
					PERR("permission denied at backend"); throw;
				} catch (Out_of_metadata) {
					if ((_alloc.quota() - _alloc.consumed()) > 8192) {
						_alloc.withdraw(8192);
						Genode::env()->parent()->upgrade(_fs.cap(), "ram_quota=8K");
						handle = _fs.symlink(dir_handle, name, create);
					} else
						throw;
				} /* TODO: dangling hash node*/

				Directory &dir_node = _node_registry.lookup_dir(dir_handle);
				_node_registry.insert(handle, dir_node.symlink(name_str, create));

				return handle;
			}

			/*
			 * Here we perform some trickery, when the client
			 * accesses a symlink at the top level path, and that
			 * symlink has the same name of a currently proccessing
			 * hash tree, the tree is finalized, and the client may
			 * find the final name by reading the symlink.
			 */
			if (!create) {
				Hash_root &root = _root_registry.lookup(name_str);
				if (!root.done)
					throw Lookup_failed();

				return root.handle();
			}

			Hash_root &root = _root_registry.lookup(name_str);
			finish(root);
			return root.handle();
		}

		/**
		 * Return nodes as usual but rewrite
		 * the first element of the path, or
		 * return a handle to a vitual symlink.
		 */
		Node_handle node(File_system::Path const &path) override
		{
			if (is_root(path))
				return _root_handle;

			char new_path[MAX_PATH_LEN];
			char const *sub_path = split_path(new_path, path.string());
			char const *root_name = new_path+1;

			Hash_root &root = _root_registry.lookup(root_name);

			/* If the root is done, pretend it is a symlink. */
			if (root.done) {
				if (*sub_path)
					throw Lookup_failed();
				return root.handle();
			}

			strncpy(new_path+1, root.filename, sizeof(new_path)-1);

			if (*sub_path) {
				size_t len = strlen(new_path);
				new_path[len++] = '/';
				strncpy(new_path+len, sub_path, sizeof(new_path)-len);
			}

			Node_handle node = _fs.node(new_path);
			return node;
		}

		void close(Node_handle handle) override
		{
			if (handle == _root_handle || handle.value & ROOT_HANDLE_PREFIX)
				return;
			_fs.close(handle);
		}

		Status status(Node_handle node_handle) override
		{
			if (node_handle.value & ROOT_HANDLE_PREFIX) {
				Hash_root &root = _root_registry.lookup(node_handle);

				return Status {
					strlen(root.filename),
					Status::MODE_SYMLINK,
					0
				};
			}

			if (node_handle != _root_handle)
				return _fs.status(node_handle);

			/*
			 * Return a stat with a zero size to trick clients
			 * into thinking that the directory can't be listed
			 * Listing the root directory is not allowed.
			 */
			Status stat = _fs.status(_root_handle);
			stat.size = 0;
			return stat;
		}

		void control(Node_handle node_handle, Control op) override { }

		/**
		 * There may be a way to trick the server if a file
		 * is opened twice for write only and read only.
		 */
		void unlink(Dir_handle dir_handle, File_system::Name const &name) override
		{
			char const *name_str = name.string();
			_fs.unlink(dir_handle, name);

			if (dir_handle == _root_handle) {
				Hash_root &root = _root_registry.lookup(name_str);

				_root_registry.remove(&root);
				return;
			}

			Hash_node *node = _node_registry.lookup_dir(dir_handle).remove(name_str);
			if (node) destroy(_alloc, node);
		}

		void truncate(File_handle file_handle, file_size_t len) override
		{
			_fs.truncate(file_handle, len);
			_node_registry.lookup_file(file_handle).truncate(len);
		}

		void move(Dir_handle from_dir_handle, File_system::Name const &from_name,
		          Dir_handle to_dir_handle,   File_system::Name const &to_name) override
		{
			if ((from_dir_handle == _root_handle)
			 || (  to_dir_handle == _root_handle))
				throw Permission_denied(); /* XXX: just being lazy */

			/* this will trigger Invalid_handle if appropriate */
			Directory &from_dir_node = _node_registry.lookup_dir(from_dir_handle);
			Directory   &to_dir_node = _node_registry.lookup_dir(  to_dir_handle);

			/* make the move now and propagate any exceptions */
			_fs.move(from_dir_handle, from_name, to_dir_handle, to_name);

			Hash_node *node = to_dir_node.remove(to_name.string());
			if (node)
				destroy(_alloc, node);

			node = from_dir_node.remove(from_name.string());
			if (!node) {
				PERR("internal state inconsistent with backend!");
				throw Permission_denied();
			}

			node->name(to_name.string());

			to_dir_node.insert(node);
		}

		bool sigh(Node_handle node_handle, Signal_context_capability sigh) override {
			return _fs.sigh(node_handle, sigh); }
};


#endif
