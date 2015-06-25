/*
 * \brief  File hashing nodes
 * \author Emery Hemingway
 * \date   2015-06-02
 */

#ifndef _STORE_IMPORT__NODE_H_
#define _STORE_IMPORT__NODE_H_

/* Genode includes. */
#include <file_system/util.h>
#include <hash/blake2s.h>
#include <util/list.h>

namespace Store_import {

	using namespace File_system;

	class Hash_node;
	class File;
	class Directory;

}

class Store_import::Hash_node : public List<Hash_node>::Element {

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

		virtual void write(uint8_t const *dst, size_t len, seek_off_t offset)
		{
			throw Invalid_handle();
		}

		void digest(uint8_t *buf, size_t len) {
			return _hash.digest(buf, len); }

};


class Store_import::File : public Hash_node
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
				/* Round to the nearest multiple of the hash block size. */
				size_t const packet_size =
					(source.bulk_buffer_size() / _hash.block_size()) * _hash.block_size() / 2;
				File_system::Packet_descriptor raw_packet =
					source.alloc_packet(packet_size);
				Packet_guard guard(source, raw_packet);

				/* Align reads with the block size. */
				size_t count = _offset % packet_size;
				if (!count) count = packet_size;

				while (_offset < size) {
					File_system::Packet_descriptor
						packet(raw_packet, 0, handle,
						       File_system::Packet_descriptor::READ,
						       count, _offset);

					source.submit_packet(packet);
					packet = source.get_acked_packet();
					if (!packet.succeeded()) {
						PERR("import flush failed");
						throw No_space();
					}
					size_t length = packet.length();
					_hash.update((uint8_t *)source.packet_content(packet), length);
					_offset += length;
					count = (size - _offset) > packet_size ?
						size - _offset : packet_size;
				}
			}

			/* Append the type and name. */
			_hash.update((uint8_t *)"\0f\0", 3);
			_hash.update((uint8_t *)name(), strlen(name()));
			_offset = 0;
		}

};


class Store_import::Directory : public Hash_node
{
	private:

		Genode::Allocator &_alloc;
		List<Hash_node>    _children;

		void destroy_children()
		{
			for (Hash_node *node = _children.first(); node; node = node->next())
				destroy(env()->heap(), node);
		}

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

			for (Hash_node *cur = _children.first(); cur; cur = cur->next()) {
				char const *cur_name = cur->name();
				for (int i = 0; i < MAX_NAME_LEN; ++i) {
					if (cur_name[i] > new_name[i])
						break;
					if (cur_name[i] < new_name[i])
						return _children.insert(node, cur);
				}
			}
			/* list was empty */
			_children.insert(node);
		}

	public:

		/**
		 * Constructor
		 */
		Directory(char const *name, Genode::Allocator &alloc)
		: Hash_node(name), _alloc(alloc) { }

		~Directory() { destroy_children(); }

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

				Directory *dir_node = dynamic_cast<Directory *>(node);
				if (dir_node) {
					strncpy(sub_path_insert, dir_node->name(), sub_name_len);
					dir_node->flush(fs, sub_path);
					dir_node->digest(buf, sizeof(buf));
					_hash.update(buf, sizeof(buf));
					continue;
				}

				/* Not really an appropriate error here, since its our fault. */
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

		void remove(char const *name)
		{
			for (Hash_node *node = _children.first();
				node; node = node->next()) {
				if (strcmp(node->name(), name, MAX_NAME_LEN))
					continue;

				_children.remove(node);
				destroy(env()->heap(), node);
				return;
			}
		}

};


#endif