/*
 * \brief  Registries for hash metadata
 * \author Emery Hemingway
 * \date   2016-03-15
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _STORE_INGEST__REGISTRY_H_
#define _STORE_INGEST__REGISTRY_H_

/* Local includes */
#include "node.h"

namespace Store_ingest {

	struct Hash_node_registry;
	struct Hash_root_registry;

	class Hash_root;

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

}


struct Store_ingest::Hash_node_registry
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

			void insert(Node_handle handle, Hash_node *node)
			{
				if (handle.value >= 0 && handle.value > MAX_NODE_HANDLES)
					throw Out_of_node_handles();

				_nodes[handle.value] = node;
			}

			Hash_node *lookup(Node_handle handle)
			{
				int i = handle.value;
				return (i >= 0 && i < MAX_NODE_HANDLES) ?
					_nodes[i] : 0;
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
struct Store_ingest::Hash_root
{
			char           filename[MAX_NAME_LEN];
			Hash_node     *hash;
			unsigned const index;
			bool           done;

			Hash_root(Hash_node *node, int index, uint64_t nonce)
			: hash(node), index(index)
			{
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


class Store_ingest::Hash_root_registry
{
	private:

				Hash_root         *_roots[MAX_ROOT_NODES];
				Genode::Allocator &_alloc;
				Genode::uint64_t   _nonce;

	public:

				Hash_root_registry(Genode::Allocator &alloc)
				: _alloc(alloc)
				{
					for (unsigned i = 0; i < MAX_ROOT_NODES; i++)
						_roots[i] = 0;

					/* use a random initial nonce */
					using namespace Jitter;
					jent_entropy_init();
					rand_data *ec = Jitter::jent_entropy_collector_alloc(0, 0);
					jent_read_entropy(ec, (char*)&_nonce, sizeof(_nonce));
					jent_entropy_collector_free(ec);
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
					for (int i = 0; i < MAX_ROOT_NODES; ++i) {
						if (_roots[i])
							continue;
						Hash_root *root = new (_alloc) Hash_root(node, i, ++_nonce);
						_roots[i] = root;
						return root;
					}
					throw Out_of_node_handles();
				}

				/**
				 * Find the root for a given name or return a null pointer.
				 */
				Hash_root *lookup(char const *name)
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
				Hash_root *lookup(Node_handle handle) {
					return _roots[handle.value&ROOT_HANDLE_MASK]; }

				void remove(Hash_root *root)
				{
					// TODO: this index thing is sloppy
					_roots[root->index] = 0;
					destroy(_alloc, root->hash);
					destroy(_alloc, root);

				}
};

#endif