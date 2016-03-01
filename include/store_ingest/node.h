/*
 * \brief  File hashing nodes
 * \author Emery Hemingway
 * \date   2015-06-02
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__STORE_IMPORT__NODE_H_
#define _INCLUDE__STORE_IMPORT__NODE_H_

/* Genode includes. */
#include <file_system/util.h>
#include <hash/blake2s.h>
#include <util/list.h>

namespace Store_ingest {

	using namespace Genode;
	using namespace File_system;

	class Hash_node;
	class File;
	class Symlink;
	class Directory;

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

class Store_ingest::Hash_node : public List<Hash_node>::Element {

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


class Store_ingest::File : public Hash_node
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
					if (!packet.succeeded()) {
						PERR("read back of node '%s' failed", name());
						throw File_system::Exception();
					}
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


class Store_ingest::Symlink : public Hash_node
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


class Store_ingest::Directory : public Hash_node
{
	private:

		Genode::Allocator_guard &_alloc;
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

		Directory *lookup_dir(char const *dir_name)
		{
			for (Hash_node *node = _children.first(); node; node = node->next()) {
				Directory *dir = dynamic_cast<Directory *>(node);
				if (dir)
					if (strcmp(dir->name(), dir_name, MAX_NAME_LEN) == 0)
						return dir;
			}
			throw Lookup_failed();
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

	public:

		/**
		 * Constructor
		 */
		Directory(char const *name, Genode::Allocator_guard &alloc)
		: Hash_node(name), _alloc(alloc) { }

		~Directory()
		{
			for (Hash_node *node = _children.first(); node; node = node->next())
				destroy(_alloc, node);
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

		Directory *dir(char const *path, bool create)
		{
			char name[MAX_NAME_LEN];
			char const *sub_path = split_path(name, path);

			Directory *dir;
			if (create && !*sub_path) {
				dir = new (_alloc) Directory(name, _alloc);
				insert(dir);
				return dir;
			}

			dir = lookup_dir(name);
			if (*sub_path)
				return dir->dir(sub_path, create);

			return dir;
		}

		File *file(char const *name, bool create)
		{
			File *file;
			if (create) {
				file = new (_alloc) File(name);
				insert(file);
				return file;
			}

			for (Hash_node *node = _children.first();
				node; node = node->next()) {
				File *file = dynamic_cast<File *>(node);
				if (file)
					if (strcmp(file->name(), name, MAX_NAME_LEN) == 0)
						return file;
			}
			throw Lookup_failed();
		}

		Symlink *symlink(char const *name, bool create)
		{
			Symlink *link;
			if (create) {
				link = new (_alloc) Symlink(name);
				insert(link);
				return link;
			}

			for (Hash_node *node = _children.first();
				node; node = node->next()) {
				Symlink *link = dynamic_cast<Symlink *>(node);
				if (link)
					if (strcmp(link->name(), name, MAX_NAME_LEN) == 0)
						return link;
			}
			throw Lookup_failed();
		}

		void remove(char const *name)
		{
			for (Hash_node *node = _children.first();
				node; node = node->next()) {
				if (strcmp(node->name(), name, MAX_NAME_LEN))
					continue;

				_children.remove(node);
				destroy(_alloc, node);
				return;
			}
		}

};

#endif
