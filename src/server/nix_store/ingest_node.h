/*
 * \brief  File hashing nodes
 * \author Emery Hemingway
 * \date   2015-06-02
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__INGEST_NODE_H_
#define _NIX_STORE__INGEST_NODE_H_

/* Genode includes. */
#include <file_system/util.h>
#include <hash/blake2s.h>
#include <util/list.h>
#include <trace/timestamp.h>

namespace Nix_store {

	using namespace Genode;
	using namespace File_system;

	class Hash_node;
	class File;
	class Symlink;
	class Directory;

	struct Hash_node_registry;
	struct Hash_root_registry;

	struct Hash_root;

	enum {
		/*
		 * Maximum number of ingest roots.
		 * The prefix and mask is used to return handles for virtual
		 * symlink nodes that do not exist on the backend.
		 */
		MAX_ROOT_NODES     = 64,
		ROOT_HANDLE_PREFIX = 0x80,
		ROOT_HANDLE_MASK   = 0X3F
	};

	/**
	 * Write the first element to name and return the start of the second path.
	 */
	char const *split_path(char *name, char const *path)
	{
		for (int i = 1; i < MAX_NAME_LEN && path[i]; ++i)
			if (path[i] == '/') {
				Genode::strncpy(name, path, ++i);
				return path+i;
			}

		size_t len = strlen(path);
		Genode::strncpy(name, path, len+1);
		return path+len;
	}

}

class Nix_store::Hash_node : public List<Hash_node>::Element {

	private:

		char _name[MAX_NAME_LEN];

	protected:

		Hash::Blake2s _hash;

	public:

		/**
		 * Constructor
		 */
		Hash_node(char const *node_name) { name(node_name); }

		char const *name() const { return _name; }

		void name(char const *name) {
			strncpy(_name, name, sizeof(_name)); }

		virtual void write(uint8_t const *dst, size_t len, seek_off_t offset) {
			throw Invalid_handle(); }

		void digest(uint8_t *buf, size_t len) {
			return _hash.digest(buf, len); }

};


class Nix_store::File : public Hash_node
{
	private:

		seek_off_t _offset; /* Last content position hashed. */

	public:

		/**
		 * Constructor
		 */
		File(char const *filename)
		: Hash_node(filename) { }

		/**
		 * Update hash with new data if it is sequential with previous data.
		 */
		void write(uint8_t const *dst, size_t len, seek_off_t offset)
		{
			if (offset > _offset)
				return;
			if (offset < _offset) {
				_offset = 0;
				_hash.reset();
			}

			_hash.update(dst, len);
			_offset += len;
		}

		void truncate(file_size_t size)
		{
			if (size >= _offset)
				return;

			_offset = 0;
			_hash.reset();
		}

		void flush(File_system::Session &fs, File_handle handle)
		{
			file_size_t size = fs.status(handle).size;

			if (size != _offset) {
				File_system::Session::Tx::Source &source = *fs.tx();
				/* try to round to the nearest multiple of the hash block size */
				size_t packet_size =
					((source.bulk_buffer_size() / _hash.block_size()) * _hash.block_size()) / 2;
				File_system::Packet_descriptor raw_packet =
					source.alloc_packet(packet_size);
				Packet_guard guard(source, raw_packet);

				while (packet_size > raw_packet.size())
					packet_size /= 2;

				/* Do a short read to align the packet stream with the block size */
				size_t n = _offset % packet_size;
				if (!n) n = packet_size;

				while (_offset < size) {
					File_system::Packet_descriptor
						packet(raw_packet, handle,
						       File_system::Packet_descriptor::READ,
						       n, _offset);

					source.submit_packet(packet);
					packet = source.get_acked_packet();
					size_t length = packet.length();
					_hash.update((uint8_t *)source.packet_content(packet), length);
					_offset += length;
					n = min(size - _offset, packet_size);
				}
			}

			/* Append the type and name. */
			_hash.update((uint8_t *)"\0f\0", 3);
			_hash.update((uint8_t *)name(), strlen(name()));
			_offset = 0;
		}
};


class Nix_store::Symlink : public Hash_node
{
	public:

		/**
		 * Constructor
		 */
		Symlink(char const *filename)
		: Hash_node(filename) { }

		/**
		 * Update hash with symlink target.
		 */
		void write(uint8_t const *dst, size_t len, seek_off_t offset)
		{
			if (offset != 0)
				return;

			_hash.reset();
			_hash.update(dst, len);
		}


		void flush(File_system::Session &fs, Symlink_handle handle)
		{
			/* Append the type and name. */
			_hash.update((uint8_t *)"\0s\0", 3);
			_hash.update((uint8_t *)name(), strlen(name()));
		}
};


class Nix_store::Directory : public Hash_node
{
	private:

		Genode::Allocator &_alloc;
		List<Hash_node>    _children;

		File *lookup_file(char const *file_name)
		{
			for (Hash_node *node = _children.first(); node; node = node->next()) {
				File *file = dynamic_cast<File *>(node);
				if (file)
					if (strcmp(file->name(), file_name, MAX_NAME_LEN) == 0)
						return file;
			}
			throw Lookup_failed();
		}

		Directory &lookup_dir(char const *dir_name)
		{
			for (Hash_node *node = _children.first(); node; node = node->next()) {
				Directory *dir = dynamic_cast<Directory *>(node);
				if (dir)
					if (strcmp(dir->name(), dir_name, MAX_NAME_LEN) == 0)
						return *dir;
			}
			throw Lookup_failed();
		}

	public:

		/**
		 * Constructor
		 */
		Directory(char const *name, Genode::Allocator &alloc)
		: Hash_node(name), _alloc(alloc) { }

		~Directory()
		{
			for (Hash_node *node = _children.first(); node; node = node->next())
				destroy(_alloc, node);
		}

		/**
		 * Insert a node into the ordered list of children.
		 */
		void insert(Hash_node *node)
		{
			char const *new_name = node->name();

			Hash_node *prev = 0;
			Hash_node *cur = _children.first();

			while (cur) {
				if (strcmp(new_name, cur->name()) < 0)
					break;
				prev = cur; cur = cur->next();
			}

			_children.insert(node, prev);
		}

		/**
		 * Remove a node from the list of children
		 */
		Hash_node *remove(char const *name)
		{
			for (Hash_node *node = _children.first();
			     node; node = node->next())
			{
				int n = strcmp(name, node->name());
				if (n < 0)
					continue;

				if (n > 0) /* not in the list */
					return 0;

				_children.remove(node);
				return node;
			}

			return 0;
		}

		void flush(File_system::Session &fs, char const *path)
		{
			uint8_t buf[_hash.size()];

			Dir_handle handle = fs.dir(path, false);
			Handle_guard guard(fs, handle);

			char sub_path[MAX_PATH_LEN];
			strncpy(sub_path, path, sizeof(sub_path));
			char *sub_path_insert = sub_path;
			int sub_name_len = MAX_PATH_LEN;
			for (; *sub_path_insert; ++sub_path_insert)
				--sub_name_len;

			*sub_path_insert = '/';
			++sub_path_insert;
			--sub_name_len;

			for (Hash_node *node = _children.first(); node; node = node->next()) {
				File *file_node = dynamic_cast<File *>(node);
				if (file_node) {
					File_handle file_handle =
						fs.file(handle, file_node->name(), READ_ONLY, false);
					Handle_guard file_guard(fs, file_handle);
					file_node->flush(fs, file_handle);
					file_node->digest(buf, sizeof(buf));
					_hash.update(buf, sizeof(buf));
					continue;
				}

				Symlink *link_node = dynamic_cast<Symlink *>(node);
				if (link_node) {
					Symlink_handle link_handle =
						fs.symlink(handle, link_node->name(), false);
					Handle_guard link_guard(fs, link_handle);
					link_node->flush(fs, link_handle);
					link_node->digest(buf, sizeof(buf));
					_hash.update(buf, sizeof(buf));
					continue;
				}

				Directory *dir_node = dynamic_cast<Directory *>(node);
				if (dir_node) {
					strncpy(sub_path_insert, dir_node->name(), sub_name_len);
					dir_node->flush(fs, sub_path);
					dir_node->digest(buf, sizeof(buf));
					_hash.update(buf, sizeof(buf));
					continue;
				}

				throw Invalid_handle();
			}

			/* Append the type and name. */
			_hash.update((uint8_t *)"\0d\0", 3);
			_hash.update((uint8_t *)name(), strlen(name()));
		}

		Directory &dir(char const *path, bool create)
		{
			char name[MAX_NAME_LEN];
			char const *sub_path = split_path(name, path);

			if (create && !*sub_path) try {
				Directory *dir = new (_alloc) Directory(name, _alloc);
				insert(dir);
				return *dir;
			} catch (Genode::Allocator::Out_of_memory) {
				throw Out_of_metadata();
			}

			Directory &dir = lookup_dir(name);
			if (*sub_path)
				return dir.dir(sub_path, create);
			return dir;
		}

		File &file(char const *name, bool create)
		{
			File *file;
			if (create) try {
				file = new (_alloc) File(name);
				insert(file);
				return *file;
			} catch (Genode::Allocator::Out_of_memory) {
				 throw Out_of_metadata();
			}

			for (Hash_node *node = _children.first();
				node; node = node->next()) {
				File *file = dynamic_cast<File *>(node);
				if (file)
					if (strcmp(file->name(), name, MAX_NAME_LEN) == 0)
						return *file;
			}
			throw Lookup_failed();
		}

		Symlink &symlink(char const *name, bool create)
		{
			Symlink *link;
			if (create) try {
				link = new (_alloc) Symlink(name);
				insert(link);
				return *link;
			} catch (Genode::Allocator::Out_of_memory) {
				 throw Out_of_metadata();
			}

			for (Hash_node *node = _children.first();
				node; node = node->next()) {
				Symlink *link = dynamic_cast<Symlink *>(node);
				if (link)
					if (strcmp(link->name(), name, MAX_NAME_LEN) == 0)
						return *link;
			}
			throw Lookup_failed();
		}
};


struct Nix_store::Hash_node_registry
{

	/* maximum number of open nodes per session */
	enum { MAX_NODE_HANDLES = 128U };

			/*
			 * A mapping of open client handle from
			 * the backend to our hashing nodes.
			 */
			Hash_node *_nodes[MAX_NODE_HANDLES];

			Hash_node_registry()
			{
				for (unsigned i = 0; i < MAX_NODE_HANDLES; ++i)
					_nodes[i] = 0;
			}

			void close_all(File_system::Session &fs)
			{
				for (unsigned i = 0; i < MAX_NODE_HANDLES; ++i)
					if (_nodes[i])
						fs.close(Node_handle(i));
			}

			void insert(Node_handle handle, Hash_node &node)
			{
				if (handle.value >= 0 && handle.value > MAX_NODE_HANDLES)
					throw Out_of_metadata();

				_nodes[handle.value] = &node;
			}

			Hash_node *lookup(Node_handle handle)
			{
				int i = handle.value;
				return (i >= 0 && i < MAX_NODE_HANDLES) ?
					_nodes[i] : nullptr;
			}

			File &lookup_file(Node_handle handle)
			{
				Hash_node *node = lookup(handle);
				File *file = dynamic_cast<File *>(node);
				if (file) return *file;
				throw Invalid_handle();
			}

			Directory &lookup_dir(Node_handle handle)
			{
				Hash_node *node = lookup(handle);
				Directory *dir = dynamic_cast<Directory *>(node);
				if (dir) return *dir;
				throw Invalid_handle();
			}
};


/**
 * Hash roots are the top-level nodes of an ingest
 * session.
 *
 * These nodes have different names on the backend
 * than the names the requested by clients. This is
 * so that stale ingests are easy to find and remove.
 *
 * Hash roots are finalized by when the client creates
 * a symlink at the virtualised location of the root.
 */
struct Nix_store::Hash_root
{
		char               name[MAX_NAME_LEN];
		char           filename[MAX_NAME_LEN];
		Hash_node     *node = nullptr;
		unsigned const index;
		bool           done = false;

		Hash_root(char const *root_name, int index, uint64_t nonce) : index(index)
		{
			strncpy(name, root_name, sizeof(name));
			snprintf(filename, sizeof(filename), "ingest-%llu", ++nonce);
		}

		Symlink_handle handle() {
			return index | ROOT_HANDLE_PREFIX; }

		void finalize(char const *name)
		{
			strncpy(filename, name, sizeof(filename));
			done = true;
		}
};


/**
 * Allocates and manages Hash_roots
 *
 * Allocation is implemented in such a way that it may be
 * interupted by an out of memory exception and later resumed.
 */
class Nix_store::Hash_root_registry
{
	private:

		Hash_root         *_roots[MAX_ROOT_NODES];
		Genode::Allocator &_alloc;

		File_system::Session &_fs;
		File_system::Dir_handle  _root_handle;

		/* use a random initial nonce */
		Genode::uint64_t _nonce = Genode::Trace::timestamp();
		bool             _strict = false;

		Hash_root *_lookup(char const *name)
		{
			/* check if the root exists*/
			for (size_t i = 0; i < MAX_ROOT_NODES; ++i)
				if (_roots[i] && (strcmp(_roots[i]->name, name, MAX_NAME_LEN) == 0))
					return _roots[i];
			return nullptr;
		}

		Hash_root &_alloc_root(char const *name)
		{
			for (int i = 0; i < MAX_ROOT_NODES; ++i) {
				if (_roots[i])
					continue;

				try {
					_roots[i] = new (_alloc) Hash_root(name, i, ++_nonce);
					return *(_roots[i]);
				} catch (Genode::Allocator::Out_of_memory) {
					throw Out_of_metadata();
				}
			}
			throw Out_of_metadata();
		}

	public:

		Hash_root_registry(Genode::Allocator &alloc,
		                   File_system::Session &fs,
		                   File_system::Dir_handle root)
		: _alloc(alloc), _fs(fs), _root_handle(root)
		{
			for (unsigned i = 0; i < MAX_ROOT_NODES; ++i)
				_roots[i] = nullptr;
		}

		~Hash_root_registry()
		{
			for (unsigned i = 0; i < MAX_ROOT_NODES; ++i)
				if (_roots[i])
					remove(*_roots[i]);
		}

		void prealloc_root(char const *name)
		{
			/* when any root is declared, go to strict mode */
			_strict = true;

			if (!_lookup(name))
				_alloc_root(name);
		}

		Hash_root &alloc_root(char const *name)
		{
			if (Hash_root *root = _lookup(name)) {
				if (_strict) {
					Genode::error(name, " is not a declared ingest root");
					throw Permission_denied();
				}
				return *root;
			}
			return _alloc_root(name);
		}

		Hash_root &alloc_dir(char const *name)
		{
			Hash_root &root = alloc_root(name);
			if (!root.node) try {
				root.node = new (_alloc) Directory(name, _alloc);
			} catch (Genode::Allocator::Out_of_memory) {
				throw Out_of_metadata();
			}
			return root;
		}

		Hash_root &alloc_file(char const *name)
		{
			Hash_root &root = alloc_root(name);
			if (!root.node) try {
				root.node = new (_alloc) File(name);
			} catch (Genode::Allocator::Out_of_memory) {
				throw Out_of_metadata();
			}
			return root;
		}

		/**
		 * Find the root for a given name
		 *
		 * \throw Lookup_failed
		 */
		Hash_root &lookup(char const *name)
		{
			if (Hash_root *root = _lookup(name))
				return *root;
			throw Lookup_failed();
		}

		/**
		 * Find the root by index
		 *
		 * \throw Lookup_failed
		 */
		Hash_root &lookup(Node_handle handle)
		{
			if (Hash_root *root = _roots[handle.value&ROOT_HANDLE_MASK])
				return *root;
			throw Lookup_failed();
		}

		void remove(Hash_root &root)
		{
			_roots[root.index] = nullptr;
			if (root.node)
				destroy(_alloc, root.node);

			if (!root.done) try {
				_fs.unlink(_root_handle, root.filename);
			} catch (...) { }

			destroy(_alloc, &root);
		}
};


#endif
