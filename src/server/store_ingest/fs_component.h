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

#ifndef _STORE_INGEST__FS_COMPONENT_H_
#define _STORE_INGEST__FS_COMPONENT_H_

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

/* Jitterentropy */
namespace Jitter { extern "C" {
#include <jitterentropy.h>
} }

/* Local includes */
#include "registry.h"
#include "node.h"

namespace Store_ingest {

	class     Fs_component;

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

}


class Store_ingest::Fs_component : public File_system::Session_rpc_object
{
	private:

		struct Output :
			Genode::String<MAX_NAME_LEN>, Genode::List<Output>::Element
		{
			Output(char const *id) : Genode::String<MAX_NAME_LEN>(id) { }
		};

		Genode::Allocator_guard  _alloc;

		/* top level hash nodes */
		Hash_root_registry _root_registry { _alloc };

		/**
		 * This registry maps node handles from the backend
		 * store to the local tree of hashing nodes.
		 */
		Hash_node_registry _registry;

		Genode::List<Output> _outputs;

		/* a queue of packets from the client awaiting backend processing */
		File_system::Packet_descriptor _packet_queue[TX_QUEUE_SIZE];

		Genode::Allocator_avl           _fs_tx_alloc;
		File_system::Connection         _fs;
		Signal_rpc_member<Fs_component> _process_packet_dispatcher;
		Dir_handle                      _root_handle;


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
			/* assume failure by default */
			theirs.succeeded(false);

			void *content = tx_sink()->packet_content(theirs);
			size_t const length = theirs.length();

			if (!content || (length > theirs.size()) || (length == 0)
			    /* Reading the entries of the root is not allowed. */
			 || !theirs.handle().valid() || theirs.handle() == _root_handle)
				return false;

			try {
				/* Emulate the read of a symlink */
				if (theirs.handle().value & ROOT_HANDLE_PREFIX) {
					Hash_root *root = _root_registry.lookup(theirs.handle());
					if (root && root->done && theirs.operation() == File_system::Packet_descriptor::READ) {
						size_t name_len = strlen(root->filename);
						if (name_len <= length) {
							memcpy(content, root->filename, name_len);
							theirs.length(name_len);
							theirs.succeeded(true);
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
					Hash_node *hash_node = _registry.lookup(ours.handle());
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
			catch (Invalid_handle)     { PERR("Invalid_handle");     }
			catch (Size_limit_reached) { PERR("Size_limit_reached"); }
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
					void *dst = tx_sink()->packet_content(theirs);
					if (!dst)
						length = 0;

					memcpy(dst, content, length);
					break;
			}
			theirs.length(length);
			theirs.succeeded(length > 0);

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

		bool not_output(char const *id)
		{
			/* no outputs defined, be permissive */
			if (!_outputs.first()) return false;

			for (Output *o = _outputs.first(); o; o = o->next())
				if (*o == id) return false;
			return true;
		}

	public:

			/**
			 * Constructor
			 *
			 * The tx buffer size is split between the local
			 * stream buffer and the backend buffer.
			 */
			Fs_component(Server::Entrypoint &ep,
			             Genode::Allocator  &alloc,
			             size_t              ram_quota,
			             size_t              tx_buf_size)
			:
				Session_rpc_object(env()->ram_session()->alloc(tx_buf_size/2), ep.rpc_ep()),
				_alloc(&alloc, ram_quota),
				_fs_tx_alloc(&_alloc),
				_fs(_fs_tx_alloc, tx_buf_size/2),
				_process_packet_dispatcher(ep, *this, &Fs_component::_process_packets)
			{
				_root_handle = _fs.dir("/", false);

				/* invalidate the packet queue */
				for (int i = 0; i < TX_QUEUE_SIZE; ++i)
					_packet_queue[i] = File_system::Packet_descriptor();

				/*
				 * Register '_process_packets' dispatch function as signal
				 * handler for client packet submission signals.
				 */
				_tx.sigh_packet_avail(_process_packet_dispatcher);
			}

		/**
		 * Destructor
		 */
		~Fs_component()
		{
			Dataspace_capability ds = tx_sink()->dataspace();
			env()->ram_session()->free(static_cap_cast<Ram_dataspace>(ds));
		}

		void upgrade_ram_quota(size_t ram_quota) { _alloc.upgrade(ram_quota); }

		/**
		 * Used by the ingest component to restrict the root nodes
		 */
		void expect(char const *id) {
			_outputs.insert(new (_alloc) Output(id)); }

		void finish(Hash_root *root)
		{
			if (root->done) return;

			_registry.close_all(_fs);

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
			root->hash->digest(&final_name[1], sizeof(final_name)-1);
			Store_hash::encode(&final_name[1], root->hash->name(), sizeof(final_name)-1);
			final_name[0] = '/';

			try {
				/* check if the final path already exists */
				_fs.close(_fs.node((char *)final_name));
				try {
					_fs.unlink(_root_handle, root->filename);
				} catch (Not_empty) {
					Genode::Path<MAX_PATH_LEN> path(root->filename);
					empty_dir(_fs, path.base());
					_fs.unlink(_root_handle, root->filename);
				}

			} catch (Lookup_failed) {
				/* move the ingest root to its final location */
				_fs.move(_root_handle, root->filename,
				         _root_handle, (char *)final_name+1);
			}

			root->finalize((char *)final_name+1);
		}

		/**
		 * Used by the ingest component to get the final name
		 */
		char const *ingest(char const *name)
		{
			Hash_root *root = _root_registry.lookup(name);
			if (!root) return "";

			finish(root);
			return root->filename;
		}

		bool verify(char const *name, char const *algo, char const *hash)
		{
			Hash_root *root = _root_registry.lookup(name);
			if (!root || !root->done)
				throw Lookup_failed();

			if (!strcmp("blake2s", algo)) {
				uint8_t buf[53];
				root->hash->digest(buf, sizeof(buf));
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


		/***************************
		 ** File_system interface **
		 ***************************/

		Dir_handle dir(File_system::Path const &path, bool create) override
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

			Hash_root *root = _root_registry.lookup(name+1);
			Directory *dir_node = 0;
			if (!root) {
				if (!create || *sub_path)
					throw Lookup_failed();

				if (not_output(name+1))
					throw Permission_denied();

				dir_node = new (_alloc) Directory(name+1, _alloc);
				try { root = _root_registry.alloc(dir_node); }
				catch (...) { destroy(_alloc, dir_node); throw; }
			} else
				dir_node = dynamic_cast<Directory *>(root->hash);

			if (!*sub_path) {
				/* Just a top level directory. */

				Dir_handle handle;
				strncpy(name+1, root->filename, sizeof(name)-1);
				try {
					handle = _fs.dir(name, create);
				} catch (Permission_denied) {
					PERR("permission denied at backend");
					throw;
				} catch (...) {
					try {
						_fs.unlink(_root_handle, name+1);
					} catch (Not_empty) {
						empty_dir(_fs, name);
						_fs.unlink(_root_handle, name+1);
					}
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

		File_handle file(Dir_handle dir_handle, File_system::Name const &name, Mode mode, bool create) override
		{
			if (create &&
			    (_alloc.quota()-_alloc.consumed() < sizeof(File)))
				throw No_space();

			File_handle handle;
			File *file_node;

			char const *name_str = name.string();

			if (dir_handle == _root_handle) {
				Hash_root *root = _root_registry.lookup(name_str);

				if (create) {
					if (root)
						throw Node_already_exists();
					else if (not_output(name_str))
						throw Permission_denied();

					file_node = new (_alloc) File(name_str);
					try { root = _root_registry.alloc(file_node); }
					catch (...) { destroy(_alloc, file_node); throw; }

				} else if (!root) {
					throw Lookup_failed();
				} else {
					file_node = dynamic_cast<File *>(root->hash);
					if (!file_node)
						throw Lookup_failed();
				}

				try {
					handle = _fs.file(_root_handle, root->filename, mode, create);
				} catch (Permission_denied) {
					PERR("permission denied at backend");
					throw;
				} catch (...) {
					try { _fs.unlink(_root_handle, root->filename); }
					catch (Not_empty) {
						Genode::Path<MAX_PATH_LEN> path(root->filename);
						empty_dir(_fs, path.base());
						_fs.unlink(_root_handle, path.base());
					}
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

			Directory &dir_node = _registry.lookup_dir(dir_handle);
			_registry.insert(handle, dir_node.file(name_str, create));
			return handle;
		}

		Symlink_handle symlink(Dir_handle dir_handle, File_system::Name const &name, bool create) override
		{
			char const *name_str = name.string();

			if (dir_handle != _root_handle) {

				if (create &&
				    (_alloc.quota()-_alloc.consumed() < sizeof(File)))
					throw No_space();

				Symlink_handle handle = _fs.symlink(dir_handle, name, create);

				Directory &dir_node = _registry.lookup_dir(dir_handle);
				_registry.insert(handle, dir_node.symlink(name_str, create));

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
				Hash_root *root = _root_registry.lookup(name_str);
				if (!root || !root->done)
					throw Lookup_failed();

				return root->handle();
			}

			Hash_root *root = _root_registry.lookup(name_str);
			if (!root)
				throw Lookup_failed();

			finish(root);

			return root->handle();
		}

		/**
		 * Return nodes as usual but rewrite
		 * the first element of the path, or
		 * return a handle to a vitual symlink.
		 */
		Node_handle node(File_system::Path const &path) override
		{
			char const *path_str = path.string();

			if (is_root(path))
				return _root_handle;

			char new_path[MAX_PATH_LEN];
			path_str = split_path(new_path, path_str);

			Hash_root *root = _root_registry.lookup(new_path+1);
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

		void close(Node_handle handle) override
		{
			if (handle == _root_handle || handle.value & ROOT_HANDLE_PREFIX)
				return;
			_fs.close(handle);
		}

		Status status(Node_handle node_handle) override
		{
			if (node_handle.value & ROOT_HANDLE_PREFIX) {
				Hash_root *root = _root_registry.lookup(node_handle);
				if (!root)
					throw Invalid_handle();

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
				Hash_root *root = _root_registry.lookup(name_str);
				if (!root)
					throw Lookup_failed();

				_root_registry.remove(root);
				return;
			}

			Hash_node *node = _registry.lookup_dir(dir_handle).remove(name_str);
			if (node) destroy(_alloc, node);
		}

		void truncate(File_handle file_handle, file_size_t len) override
		{
			_fs.truncate(file_handle, len);
			_registry.lookup_file(file_handle).truncate(len);
		}

		void move(Dir_handle from_dir_handle, File_system::Name const &from_name,
		          Dir_handle to_dir_handle,   File_system::Name const &to_name) override
		{
			if ((from_dir_handle == _root_handle)
			 || (  to_dir_handle == _root_handle))
				throw Permission_denied(); /* XXX: just being lazy */

			/* this will trigger Invalid_handle if appropriate */
			Directory &from_dir_node = _registry.lookup_dir(from_dir_handle);
			Directory   &to_dir_node = _registry.lookup_dir(  to_dir_handle);

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

		void sigh(Node_handle node_handle, Signal_context_capability sigh) override {
			_fs.sigh(node_handle, sigh); }
};


#endif
