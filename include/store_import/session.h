/*
 * \brief  File system hashing proxy
 * \author Emery Hemingway
 * \date   2015-05-28
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__STORE_IMPORT__SESSION_H_
#define _INCLUDE__STORE_IMPORT__SESSION_H_

/* Genode includes */
#include <store_hash/encode.h>
#include <file_system_session/connection.h>
#include <file_system_session/rpc_object.h>
#include <os/signal_rpc_dispatcher.h>
#include <os/server.h>
#include <os/session_policy.h>
#include <util/string.h>
#include <base/allocator_avl.h>
#include <base/allocator_guard.h>

/* Local includes */
#include "node.h"

namespace Store_import {

	using namespace File_system;

	class Session_component;

	static bool is_root(File_system::Path const &path) {
		return (path.size() == 2) && (*path.string() == '/'); }

	static unsigned long nonce()
	{
		static unsigned i;
		return i++;
	};

}

class Store_import::Session_component : public Session_rpc_object
{
	private:

		enum {

			/* maximum number of open nodes per session */
			MAX_NODE_HANDLES = 128U,

			/*
			 * Maximum number of import roots.
			 * The prefix and mask is used to return handles for virtual
			 * symlink nodes that do not exist on the backend.
			 */
			MAX_ROOT_NODES = 64,
			ROOT_HANDLE_PREFIX = 0x80,
			ROOT_HANDLE_MASK = 0X3F
		};

		Genode::Allocator_guard              _alloc;

		/**
		 * This registry maps node handles from the backend
		 * store to the local tree of hashing nodes.
		 */
		struct Registry
		{
			/*
			 * A mapping of open client handle from
			 * the backend to our hashing nodes.
			 */
			Hash_node *_nodes[MAX_NODE_HANDLES];

			Registry()
			{
				for (unsigned i = 0; i < MAX_NODE_HANDLES; ++i)
					_nodes[i] = 0;
			}

			void insert(Node_handle handle, Hash_node *node)
			{
				if (handle.value >= 0 && handle.value > MAX_NODE_HANDLES)
					throw Out_of_node_handles();

				_nodes[handle.value] = node;
			}

			Hash_node *lookup(Node_handle handle)
			{
				int i = handle.value;
				if (i >= 0 && i < MAX_NODE_HANDLES) {
					Hash_node *node = _nodes[i];
					if (node)
						return node;
					else
						return 0;
				}
				throw Invalid_handle();
			}

			File *lookup_file(Node_handle handle)
			{
				Hash_node *node = lookup(handle);
				if (node) {
					File *file = dynamic_cast<File *>(node);
					if (file) return file;
				}
				throw Invalid_handle();
			}

			Directory *lookup_dir(Node_handle handle)
			{
				Hash_node *node = lookup(handle);
				if (node) {
					Directory *dir = dynamic_cast<Directory *>(node);
					if (dir) return dir;
				}
				throw Invalid_handle();
			}

		} _registry;

		/**
		 * Hash roots are the top-level nodes of this
		 * import session.
		 *
		 * These nodes have different names on the backend
		 * than the names the requested by clients. This is
		 * so that stale imports are easy to find and remove.
		 *
		 * Hash roots are finalized by when the client creates
		 * a symlink at their virtualised location.
		 */
		struct Hash_root
		{
			char           filename[MAX_NAME_LEN];
			Hash_node     *hash;
			unsigned const index;
			bool           done;

			Hash_root(Hash_node *node)
			: hash(node), index(nonce())
			{
				if (index > MAX_ROOT_NODES)
					throw Out_of_node_handles();

				snprintf(filename, sizeof(filename), "import-%d", index);
			}

			Symlink_handle handle() {
				return index | ROOT_HANDLE_PREFIX; }

			void finalize(char const *name)
			{
				strncpy(filename, name, sizeof(filename));
				done = true;
			}
		};

		class Hash_root_registry
		{
			private:

				Hash_root         *_roots[MAX_ROOT_NODES];
				Genode::Allocator &_alloc;

			public:

				Hash_root_registry(Genode::Allocator &alloc)
				: _alloc(alloc)
				{
					for (unsigned i = 0; i < MAX_ROOT_NODES; i++)
						_roots[i] = 0;
				}

				~Hash_root_registry()
				{
					for (unsigned i = 0; i < MAX_ROOT_NODES; i++)
						if (_roots[i]) {
							destroy(_alloc, _roots[i]->hash);
							destroy(_alloc, _roots[i]);
						}
				}

				Hash_root *alloc(Hash_node *node)
				{
					Hash_root *root = new (_alloc) Hash_root(node);
					_roots[root->index] = root;
					return root;
				}

				/**
				 * Find the root for a given name or return a null pointer.
				 */
				Hash_root *lookup_name(char const *name)
				{
					for (size_t i = 0; i < MAX_ROOT_NODES; ++i) {
						Hash_root *root = _roots[i];
						if (root && (Genode::strcmp(root->hash->name(), name, MAX_NAME_LEN) == 0))
							return root;
					}
					return 0;
				}

				/**
				 * Find the root by index.
				 */
				Hash_root *lookup_index(Node_handle handle) {
					return _roots[handle.value&ROOT_HANDLE_MASK]; }

				void remove(Hash_root *root)
				{
					// TODO: this index thing is sloppy
					_roots[root->index] = 0;
				}

		} _root_registry;

		Genode::Allocator_avl                _fs_tx_alloc;
		File_system::Connection              _fs;
		Signal_rpc_member<Session_component> _process_packet_dispatcher;
		Lock                                 _packet_lock;
		Dir_handle                           _root_handle;

		/******************************
		 ** Packet-stream processing **
		 ******************************/

		/**
		 * Process and incoming client packet, return true if a
		 * response is needed from the backend.
		 */
		bool _process_incoming_packet(File_system::Packet_descriptor &theirs)
		{
			/* assume failure by default */
			theirs.succeeded(false);

			try {
				void *content = tx_sink()->packet_content(theirs);
				size_t const length = theirs.length();

				if (!content || (length > theirs.size()))
					return false;

				if (!length)
					return false;

				/* Reading the entries of the root is not allowed. */
				if (theirs.handle() == _root_handle)
					return false;

				/* Emulate the read of a symlink */
				if (theirs.handle().value & ROOT_HANDLE_PREFIX) {
					Hash_root *root = _root_registry.lookup_index(theirs.handle());
					if (root && root->done && theirs.operation() == File_system::Packet_descriptor::READ) {
						size_t name_len = strlen(root->filename);
						if (name_len <= length) {
							memcpy(content, root->filename, name_len);
							theirs.length(name_len);
							theirs.succeeded(true);
						}
					}
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
					     (Packet_ref *)&theirs,
					     theirs.handle(),
					     op,
					     length,
					     theirs.position());

				if (op == File_system::Packet_descriptor::WRITE) {
					Hash_node *hash_node = _registry.lookup(ours.handle());
					if (!hash_node) {
						/*
						 * If we don't hash it, they don't write it.
						 */
						PERR("no hash node found for handle on client packet");
						return false;
					}
					memcpy(source.packet_content(ours), content, length);
				}
				source.submit_packet(ours);
				return true;
			}
			catch (Invalid_handle)     { PERR("Invalid_handle");     }
			catch (Size_limit_reached) { PERR("Size_limit_reached"); }
			return false;
		}

		void _process_outgoing_packet()
		{
			using namespace File_system;

			File_system::Session::Tx::Source &source = *_fs.tx();
			File_system::Packet_descriptor ours = source.get_acked_packet();
			File_system::Packet_descriptor *theirs =
				(File_system::Packet_descriptor *)ours.ref();
			if (!theirs) {
				PERR("null packet reference received from the backend");
				source.release_packet(ours);
			}

			size_t length = ours.length();
			uint8_t const *content = (uint8_t const *)source.packet_content(ours);
			if (!content) {
				tx_sink()->acknowledge_packet(*theirs);
				source.release_packet(ours);
				return;
			}

			switch (ours.operation()) {
				case File_system::Packet_descriptor::WRITE:
					try {
						Hash_node *hash_node = _registry.lookup(ours.handle());
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
					void *dst = tx_sink()->packet_content(*theirs);
					if (!dst)
						length = 0;

					memcpy(dst, content, length);
					break;
			}
			theirs->length(length);
			theirs->succeeded(length > 0);

			tx_sink()->acknowledge_packet(*theirs);
			source.release_packet(ours);
		}

		/**
		 * Called by signal dispatcher, executed in the context of the main
		 * thread (not serialized with the RPC functions).
		 */
		void _process_packets(unsigned)
		{
			/*
			 * Keep packets from getting sent to the backend
			 * from the RPC thread.
			 */
			Lock::Guard packet_guard(_packet_lock);

			File_system::Packet_descriptor queue[TX_QUEUE_SIZE];
			int n = 0;

			while (tx_sink()->packet_avail() && n < TX_QUEUE_SIZE) {
				queue[n] = tx_sink()->get_packet();
				if (_process_incoming_packet(queue[n])) ++n;
				else {
					tx_sink()->acknowledge_packet(queue[n]);
				}
			}

			/*
			 * Block while waiting on the server.
			 * Not great but I haven't found an
			 * altenative to creating another
			 * thread.
			 */

			while (n--)
				_process_outgoing_packet();
		}

	public:

			/**
			 * Constructor
			 *
			 * The tx buffer size is split between the local
			 * stream buffer and the backend buffer.
			 */
			Session_component(size_t ram_quota, size_t tx_buf_size, Server::Entrypoint &ep)
			:
				Session_rpc_object(env()->ram_session()->alloc(tx_buf_size/2), ep.rpc_ep()),
				_alloc(env()->heap(), ram_quota),
				_root_registry(_alloc),
				_fs_tx_alloc(&_alloc),
				_fs(_fs_tx_alloc, tx_buf_size/2),
				_process_packet_dispatcher(ep, *this, &Session_component::_process_packets)
			{
				_root_handle = _fs.dir("/", false);

				/*
				 * Register '_process_packets' dispatch function as signal
				 * handler for client packet submission signals.
				 */
				_tx.sigh_packet_avail(_process_packet_dispatcher);
			}

		/**
		 * Destructor
		 */
		~Session_component()
		{
			Dataspace_capability ds = tx_sink()->dataspace();
			env()->ram_session()->free(static_cap_cast<Ram_dataspace>(ds));
		}

		void upgrade_ram_quota(size_t ram_quota) { _alloc.upgrade(ram_quota); }

		void finish(Hash_root *root, char const *name)
		{
			/*
			 * Prevent the main thread from sending packets to the backend
			 * while we flush here on the RPC thread.
			 */
			Lock::Guard packet_guard(_packet_lock);

			/* Flush the root. */
			File *file_node = dynamic_cast<File *>(root->hash);
			if (file_node) {
				File_handle final_handle = _fs.file(_root_handle, root->filename, READ_ONLY, false);
				Handle_guard guard(_fs, final_handle);
				file_node->flush(_fs, final_handle);
			} else {
				Directory *dir_node = dynamic_cast<Directory *>(root->hash);
				if (!dir_node)
					throw Invalid_handle();

				char path[MAX_NAME_LEN+1];
				*path = '/';
				strncpy(path+1, root->filename, sizeof(path)-1);

				dir_node->flush(_fs, path);
			}

			uint8_t final_name[MAX_NAME_LEN];
			root->hash->digest(final_name, sizeof(final_name));
			Store_hash::encode(final_name, name, sizeof(final_name));
			try {
				_fs.move(_root_handle, root->filename,
				         _root_handle, (char *)final_name);
			} catch (File_system::Node_already_exists) {
				/* A redundant import. */
				_fs.unlink(_root_handle, root->filename);
				/* This needs to be done recursively. */
			}
			/*
			 * Close all the open handles
			 * underneath the the root, unless there is
			 * a better way to prevent further writes.
			 */
			root->finalize((char *)final_name);
		}

		void finish(const char *name)
		{
			Hash_root *root = _root_registry.lookup_name(name);
			if (!root)
				throw Lookup_failed();
			finish(root, name);
		}

		char const *final(char const *name)
		{
			Hash_root *root = _root_registry.lookup_name(name);
			if (!root || !root->done)
				throw Lookup_failed();

			return root->filename;
		}


		/***************************
		 ** File_system interface **
		 ***************************/

		Dir_handle dir(File_system::Path const &path, bool create)
		{
			if (create &&
			    (_alloc.quota()-_alloc.consumed() < sizeof(Directory)))
				throw No_space();

			char const *path_str = path.string();

			if (is_root(path)) {
				if (create) throw Node_already_exists();
				else        return _root_handle;
			}

			char name[MAX_NAME_LEN+1];
			char const *sub_path = split_path(name, path_str);

			Hash_root *root = _root_registry.lookup_name(name+1);
			Directory *dir_node = 0;
			if (!root) {
				if (!create || *sub_path)
					throw Lookup_failed();

				dir_node = new (_alloc) Directory(name+1, _alloc);
				 root = _root_registry.alloc(dir_node);
			} else
				dir_node = dynamic_cast<Directory *>(root->hash);

			if (!*sub_path) {
				/* Just a top level directory. */
				Dir_handle handle;
				strncpy(name+1, root->filename, sizeof(name)-1);
				try {
					handle = _fs.dir(name, create);
				} catch (Node_already_exists) {
					_fs.unlink(_root_handle, name);
					handle = _fs.dir(name, true);
				}
				_registry.insert(handle, dir_node);
				return handle;
			}

			/* Rewrite the directory path. */
			char new_path[MAX_PATH_LEN];
			*new_path = '/';
			strncpy(new_path+1, root->filename, sizeof(new_path)-1);
			size_t len = strlen(new_path);
			new_path[len++] = '/';
			strncpy(new_path+len, sub_path, sizeof(new_path)-len);

			/* Open the directory and walk down the hash node tree. */
			Dir_handle handle = _fs.dir(new_path, create);
			_registry.insert(handle, dir_node->dir(sub_path, create));
			return handle;
		}

		File_handle file(Dir_handle dir_handle, Name const &name, Mode mode, bool create)
		{
			if (create &&
			    (_alloc.quota()-_alloc.consumed() < sizeof(File)))
				throw No_space();

			char const *name_str = name.string();

			File_handle handle;
			File *file_node;
			if (dir_handle == _root_handle) {
				Hash_root *root = _root_registry.lookup_name(name_str);
				if (!root) {
					file_node = new (_alloc) File(name_str);
					root = _root_registry.alloc(file_node);
				} else
					file_node = dynamic_cast<File *>(root->hash);

				try {
					handle = _fs.file(_root_handle, root->filename, mode, create);
				} catch (...) {
					_fs.unlink(_root_handle, root->filename);
					handle = _fs.file(_root_handle, root->filename, mode, true);
				}

				if (mode >= WRITE_ONLY)
					_registry.insert(handle, file_node);

				return handle;
			}

			handle = _fs.file(dir_handle, name, mode, create);

			/*
			 * If this node can't be used to modify data,
			 * then it is not a node we are concerned with.
			 */
			if (mode < WRITE_ONLY)
				return handle;

			Directory *dir_node = _registry.lookup_dir(dir_handle);
			_registry.insert(handle, dir_node->file(name_str, create));
			return handle;
		}

		Symlink_handle symlink(Dir_handle dir_handle, Name const &name, bool create)
		{
			char const *name_str = name.string();

			if (dir_handle != _root_handle) {

				if (create &&
				    (_alloc.quota()-_alloc.consumed() < sizeof(File)))
					throw No_space();

				Symlink_handle handle = _fs.symlink(dir_handle, name, create);

				Directory *dir_node = _registry.lookup_dir(dir_handle);
				_registry.insert(handle, dir_node->symlink(name_str, create));

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
				Hash_root *root = _root_registry.lookup_name(name_str);
				if (!root || !root->done)
					throw Lookup_failed();

				return root->handle();
			}

			Hash_root *root = _root_registry.lookup_name(name_str);
			if (!root)
				throw Lookup_failed();

			finish(root, name_str);

			return root->handle();
		}

		/**
		 * Return nodes as usual but rewrite
		 * the first element of the path, or
		 * return a handle to a vitual symlink.
		 */
		Node_handle node(File_system::Path const &path)
		{
			char const *path_str = path.string();

			if (is_root(path))
				return _root_handle;

			char new_path[MAX_PATH_LEN];
			path_str = split_path(new_path, path_str);

			Hash_root *root = _root_registry.lookup_name(new_path+1);
			if (!root)
				throw Lookup_failed();

			/* If the root is done, pretend it is a symlink. */
			if (root->done) {
				return root->handle();
			}

			strncpy(new_path+1, root->filename, sizeof(new_path)-1);

			if (*path_str) {
				size_t len = strlen(new_path);
				new_path[len++] = '/';
				strncpy(new_path+len, path_str, sizeof(new_path)-len);
			}

			return _fs.node(new_path);
		}

		void close(Node_handle handle)
		{
			if (handle != _root_handle)
				_fs.close(handle);
		}

		Status status(Node_handle node_handle)
		{
			if (node_handle.value & ROOT_HANDLE_PREFIX) {
				Hash_root *root = _root_registry.lookup_index(node_handle);
				if (!root) {
					throw Invalid_handle();
				}

				return Status {
					strlen(root->filename),
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

		void control(Node_handle node_handle, Control op) {
			return _fs.control(node_handle, op); }

		/**
		 * There may be a way to trick the server if a file
		 * is opened twice for write only and read only.
		 */
		void unlink(Dir_handle dir_handle, Name const &name)
		{
			char const *name_str = name.string();
			_fs.unlink(dir_handle, name);

			if (dir_handle == _root_handle) {
				Hash_root *root = _root_registry.lookup_name(name_str);
				if (!root)
					throw Lookup_failed();
				_root_registry.remove(root);
				destroy(_alloc, root->hash);
				destroy(_alloc, root);
				return;
			}

			_registry.lookup_dir(dir_handle)->remove(name_str);
		}

		void truncate(File_handle file_handle, file_size_t len)
		{
			_fs.truncate(file_handle, len);
			_registry.lookup_file(file_handle)->truncate(len);
		}

		void move(Dir_handle from_dir_handle, Name const &from_name,
		          Dir_handle to_dir_handle,   Name const &to_name)
		{
			PERR("move not implemented");
			/*
			 * Not hard to implement, just lazy.
			 * Need a remove and and insert function at Hash_node.
			 */
			throw Permission_denied();
		}

		void sigh(Node_handle node_handle, Signal_context_capability sigh) {
			_fs.sigh(node_handle, sigh); }

		void sync() { _fs.sync(); }
};

#endif
