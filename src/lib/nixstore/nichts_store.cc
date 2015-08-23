/*
 * \brief  Nix Store-api to Genode sessions glue
 * \author Emery Hemingway
 * \date   2015-05-27
 */

/* Upstream Nix includes. */
#include "nichts_store.h"
#include <libutil/util.hh>

/* Genode includes. */
#include <store_import/connection.h>
#include <store_hash/encode.h>
#include <hash/blake2s.h>
#include <vfs/file_system_factory.h>
#include <os/config.h>
#include <base/printf.h>


#define NOT_IMP PERR("%s not implemented", __func__)


using namespace nix;


static string hash_text(const string &name, const string &text)
{
	::Hash::Blake2s hash;
	uint8_t         buf[Builder::MAX_NAME_LEN];

	hash.update((uint8_t*)text.data(), text.size());
	hash.update((uint8_t *)"\0f\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, sizeof(buf));
	Store_hash::encode(buf, name.c_str(), sizeof(buf));

	return (char *)buf;
}


static nix::Path finalize_import(File_system::Session &fs, char const *name)
{
	using namespace File_system;

	Symlink_handle link_handle;
	try {
		Dir_handle root = fs.dir("/", false);
		Handle_guard root_guard(fs, root);
		link_handle = fs.symlink(root, name, true);
	} catch (...) {
		PERR("closing export handle for ‘%s’", name);
		throw;
	}
	Handle_guard link_guard(fs, link_handle);

	File_system::Session::Tx::Source &source = *fs.tx();

	File_system::Packet_descriptor
		packet(source.alloc_packet(MAX_NAME_LEN), 0,
		       link_handle, File_system::Packet_descriptor::READ,
		       MAX_NAME_LEN, 0);
	Packet_guard packet_guard(source, packet);

	source.submit_packet(packet);
	packet = source.get_acked_packet();

	if (!packet.succeeded())
		throw nix::Error(format("finalising import ‘%1%’") % name);
	return nix::Path(source.packet_content(packet), packet.length());
}


void Store_client::copy_dir(File_system::Session   &fs,
                            File_system::Dir_handle export_dir,
                            nix::Path const        &src_path,
                            nix::Path const        &dst_path)
{
	("%s", src_path.c_str());

	using namespace Vfs;
	using namespace File_system;

	Directory_service::Dirent dirent;

	for (file_offset i = 0;; ++i) {
		_vfs_root.dirent(src_path.c_str(), i, dirent);
		if (dirent.type == Directory_service::DIRENT_TYPE_END) break;

		Path sub_src_path = src_path + "/";
		sub_src_path.append(dirent.name);

		Path sub_dst_path = dst_path + "/";
		sub_dst_path.append(dirent.name);

		switch (dirent.type) {
		case Directory_service::DIRENT_TYPE_DIRECTORY: {
			File_system::Dir_handle sub_handle;
			try {
				sub_handle = fs.dir(sub_dst_path.c_str(), true);
			} catch (...) {
				PERR("error opening export directory handle for ‘%s’", sub_dst_path.c_str());
				throw;
			}
			File_system::Handle_guard sub_guard(fs, sub_handle);

			copy_dir(fs, sub_handle, sub_src_path, sub_dst_path);
			break;
		}
		
		case Directory_service::DIRENT_TYPE_FILE: {
			File_system::File_handle file_handle;
			try {
				file_handle = fs.file(export_dir, dirent.name,
				                      File_system::WRITE_ONLY, true);
			} catch (...) {
				PERR("error opening export file handle for ‘%s’", sub_dst_path.c_str());
				throw;
			}
			File_system::Handle_guard sub_guard(fs, file_handle);

			copy_file(fs, file_handle, sub_src_path, sub_dst_path);
			break;
		}
		
		case Directory_service::DIRENT_TYPE_SYMLINK: {
			File_system::Symlink_handle link_handle;
			try {
				link_handle = fs.symlink(export_dir, dirent.name, true);
			} catch (...) {
				PERR("error opening export symlink handle for ‘%s’", sub_dst_path.c_str());
				throw;
			}
			File_system::Handle_guard sub_guard(fs, link_handle);

			copy_symlink(fs, link_handle, sub_src_path, sub_dst_path);
			break;
		}
		
		default:
			PERR("skipping unexportable file %s", sub_src_path.c_str());
		}
	}
}


void Store_client::copy_file(File_system::Session    &fs,
                             File_system::File_handle file_handle,
                             Path const              &src_path,
                             Path const              &dst_path)
{
	using namespace Vfs;

	Directory_service::Stat stat = status(src_path);
	file_size remaining_count    = stat.size;
	file_size seek_offset        = 0;

	Vfs_handle *vfs_handle = nullptr;
	if (_vfs_root.open(src_path.c_str(),
	                   Directory_service::OPEN_MODE_RDONLY,
	                   &vfs_handle) != Directory_service::OPEN_OK)
		throw Error(format("getting handle on file ‘%1%’") % dst_path);
	Vfs_handle::Guard vfs_guard(vfs_handle);

	/* Preallocate the file space. */
	fs.truncate(file_handle, remaining_count);

	File_system::Session::Tx::Source &source = *fs.tx();
	file_size const max_packet_size = source.bulk_buffer_size();

	while (remaining_count) {
		collect_acknowledgements(source);
		size_t const curr_packet_size = std::min(remaining_count, max_packet_size);

		File_system::Packet_descriptor
			packet(source.alloc_packet(curr_packet_size),
			       0,
			       file_handle,
			       File_system::Packet_descriptor::WRITE,
			       0, seek_offset);
		File_system::Packet_guard packet_guard(source, packet);

		/* Read from the VFS to a packet. */
		file_size vfs_count;
		if (vfs_handle->fs().read(vfs_handle, source.packet_content(packet),
		                          curr_packet_size, vfs_count) != File_io_service::READ_OK)
			throw Error(format("reading file ‘%1%’") % dst_path);

		packet.length(vfs_count);

		/* pass packet to server side */
		source.submit_packet(packet);

		packet = source.get_acked_packet();
		if (!packet.succeeded())
			throw nix::Error(format("writing file ‘%1%’") % dst_path);

		/* prepare next iteration */
		remaining_count -= packet.length();
		if (remaining_count) {
			seek_offset += packet.length();
			vfs_handle->seek(seek_offset);
		}
	}
}


void Store_client::copy_symlink(File_system::Session       &fs,
                                File_system::Symlink_handle symlink_handle,
                                nix::Path const            &src_path,
                                nix::Path const            &dst_path)
{
	using namespace Vfs;

	File_system::Session::Tx::Source &source = *fs.tx();
	bool success = false;
	collect_acknowledgements(source);
	File_system::Packet_descriptor
		packet(source.alloc_packet(File_system::MAX_PATH_LEN),
		       0,
		       symlink_handle,
		       File_system::Packet_descriptor::WRITE,
		       0, 0);
	File_system::Packet_guard packet_guard(source, packet);

	/* Read from the VFS to a packet. */
	file_size vfs_count;
	if (_vfs_root.readlink(src_path.c_str(), source.packet_content(packet),
	                       packet.size(), vfs_count) != Directory_service::READLINK_OK)
		throw Error(format("reading symlink ‘%1%’") % src_path);
	packet.length(vfs_count);

	/* pass packet to server side */
	source.submit_packet(packet);

	packet = source.get_acked_packet();
	if (!packet.succeeded())
		throw Error(format("writing symlink ‘%1%’") % src_path);
}


void
Store_client::hash_file(uint8_t *buf, nix::Path const &src_path)
{
	using namespace Vfs;

	::Hash::Blake2s hash;
	file_size       remaining;
	file_size       pos = 0;

	Directory_service::Stat stat = status(src_path);

	uint8_t data[Genode::min(4096, stat.size)];

	Vfs_handle *vfs_handle = nullptr;
	if (_vfs_root.open(src_path.c_str(),
	                   Directory_service::OPEN_MODE_RDONLY,
	                   &vfs_handle) != Directory_service::OPEN_OK)
		throw Error(format("getting handle on file ‘%1%’") % src_path);
	Vfs_handle::Guard vfs_guard(vfs_handle);

	while (pos < stat.size) {
		file_size n;
		if (vfs_handle->fs().read(vfs_handle, (char *)data,
	                          sizeof(data), n) != File_io_service::READ_OK)
		throw Error(format("hashing file ‘%1%’") % src_path);

		hash.update(data, n);
		pos += n;
		vfs_handle->seek(pos);
	}

	string name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);

	hash.update((uint8_t*)"\0f\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}

void
Store_client::hash_symlink(uint8_t *buf, nix::Path const &src_path)
{
	using namespace Vfs;

	::Hash::Blake2s hash;
	uint8_t         data[File_system::MAX_PATH_LEN];

	file_size vfs_count;
	if (_vfs_root.readlink(src_path.c_str(), (char *)data,
	                       sizeof(data), vfs_count) != Directory_service::READLINK_OK)
		throw Error(format("reading symlink ‘%1%’") % src_path);

	hash.update(data, vfs_count);

	string name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);

	hash.update((uint8_t*)"\0s\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}

string
Store_client::add_file(nix::Path const &src_path)
{
	using namespace Vfs;

	Directory_service::Stat stat = status(src_path);
	file_size remaining = stat.size;;
	file_size offset    = 0;

	Vfs_handle *vfs_handle = nullptr;
	if (_vfs_root.open(src_path.c_str(),
	                   Directory_service::OPEN_MODE_RDONLY,
	                   &vfs_handle) != Directory_service::OPEN_OK)
		throw Error(format("getting handle on file ‘%1%’") % src_path);
	Vfs_handle::Guard vfs_guard(vfs_handle);

	Genode::Allocator_avl fs_block_alloc(Genode::env()->heap());
	Store_import::Connection fs(fs_block_alloc);

	nix::Path name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);
	File_system::File_handle export_handle;
	try {
		File_system::Dir_handle root_handle = fs.dir("/", false);
		File_system::Handle_guard root_guard(fs, root_handle);

		export_handle = fs.file(root_handle, name.c_str(),
		                        File_system::WRITE_ONLY, true);
	} catch (...) {
		PERR("erro opening export file handle for ‘%s’", name.c_str());
		throw;
	}
	File_system::Handle_guard fs_guard(fs, export_handle);

	File_system::Session::Tx::Source &source = *fs.tx();
	size_t const max_packet_size = source.bulk_buffer_size() / 2;

	while (remaining) {
		vfs_handle->seek(offset);
		size_t const curr_packet_size = Genode::min(remaining, max_packet_size);

		File_system::Packet_descriptor
			packet(source.alloc_packet(curr_packet_size), 0,
			       export_handle, File_system::Packet_descriptor::WRITE,
			       0, offset);
		File_system::Packet_guard guard(source, packet);

		file_size vfs_count;
		if (vfs_handle->fs().read(vfs_handle, source.packet_content(packet),
		                          curr_packet_size, vfs_count) != File_io_service::READ_OK)
			throw Error(format("reading file ‘%1%’") % src_path);

		packet.length(vfs_count);

		source.submit_packet(packet);
		packet = source.get_acked_packet();
		if (!packet.succeeded())
			throw nix::Error(format("addPathToStore: writing `%1%' failed") % src_path);
		remaining -= packet.length();
		offset    += packet.length();
	}

	return finalize_import(fs, name.c_str());
}


void
Store_client::hash_dir(uint8_t *buf, nix::Path const &src_path)
{
	using namespace Vfs;

	::Hash::Blake2s           hash;
	Directory_service::Dirent dirent;

	/* Use a map so that entries are sorted. */
	std::map<string, unsigned char> entries;

	for (file_offset i = 0;; ++i) {
		_vfs_root.dirent(src_path.c_str(), i, dirent);
		if (dirent.type == Directory_service::DIRENT_TYPE_END) break;

		entries.insert(
			std::pair<string, Directory_service::Dirent_type>
			(dirent.name, dirent.type));
	}

	for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
		Path subpath = src_path + "/" + i->first;

		if (i->second == Directory_service::DIRENT_TYPE_DIRECTORY) {
			hash_dir(buf, subpath);
			hash.update(buf, hash.size());

		} else if (i->second == Directory_service::DIRENT_TYPE_FILE) {
			hash_file(buf, subpath);
			hash.update(buf, hash.size());

		} else if (i->second == Directory_service::DIRENT_TYPE_SYMLINK) {
			hash_symlink(buf, subpath);
			hash.update(buf, hash.size());

		} else {
			PERR("unhandled file type for %s", subpath.c_str());
		}
	}

	string name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);

	hash.update((uint8_t*)"\0d\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}


string
Store_client::add_dir(nix::Path const &src_path)
{
	Genode::Allocator_avl fs_block_alloc(Genode::env()->heap());
	Store_import::Connection fs(fs_block_alloc);

	/* The index of the begining of the last path element. */
	int path_offset = src_path.rfind("/");
	Path dst_path = src_path.substr(path_offset, src_path.size());

	File_system::Dir_handle export_dir;
	try {
		export_dir = fs.dir(dst_path.c_str(), true);
	} catch (...) {
		PERR("opening export directory handle for ‘%s'", dst_path.c_str());
		throw;
	}
	File_system::Handle_guard dir_guard(fs, export_dir);

	copy_dir(fs, export_dir, src_path, dst_path);

	return finalize_import(fs, dst_path.c_str()+1);
}


/************************
 ** StoreAPI interface **
************************/

/**
 * Check whether a path is valid.
 */
bool
nix::Store_client::isValidPath(const nix::Path & path)
{
	// TODO just fix the damn double slash already
	for (int i = 0; i < path.size(); ++i)
		if (path[i] != '/')
			return _builder.valid(path.c_str()+i);
}


/* Query which of the given paths is valid. */
PathSet
Store_client::queryValidPaths(const PathSet & paths) {
			NOT_IMP; return PathSet(); };

/* Query the set of all valid paths. */
PathSet nix::Store_client::queryAllValidPaths() {
	NOT_IMP; return PathSet(); };

/* Query information about a valid path. */
ValidPathInfo nix::Store_client::queryPathInfo(const Path & path) {
	NOT_IMP; return ValidPathInfo(); };

/* Query the hash of a valid path. */
nix::Hash nix::Store_client::queryPathHash(const Path & path) {
	NOT_IMP; return Hash(); };

/* Query the set of outgoing FS references for a store path.	The
	 result is not cleared. */
void nix::Store_client::queryReferences(const Path & path,
		PathSet & references) { NOT_IMP; };

	/* Queries the set of incoming FS references for a store path.
	 The result is not cleared. */
void nix::Store_client::queryReferrers(const Path & path,
		PathSet & referrers) { NOT_IMP; };

/* Query the deriver of a store path.	Return the empty string if
	 no deriver has been set. */
Path nix::Store_client::queryDeriver(const Path & path) {
	NOT_IMP; return Path(); };

/* Return all currently valid derivations that have `path' as an
	 output.	(Note that the result of `queryDeriver()' is the
	 derivation that was actually used to produce `path', which may
	 not exist anymore.) */
PathSet nix::Store_client::queryValidDerivers(const Path & path) {
	NOT_IMP; return PathSet(); };

/* Query the outputs of the derivation denoted by `path'. */
PathSet nix::Store_client::queryDerivationOutputs(const Path & path) {
	NOT_IMP; return PathSet(); };

/* Query the output names of the derivation denoted by `path'. */
StringSet nix::Store_client::queryDerivationOutputNames(const Path & path) {
	NOT_IMP; return StringSet(); };

/* Query the full store path given the hash part of a valid store
	 path, or "" if the path doesn't exist. */
Path nix::Store_client::queryPathFromHashPart(const string & hashPart) {
	NOT_IMP; return Path(); };

/* Query which of the given paths have substitutes. */
PathSet nix::Store_client::querySubstitutablePaths(const PathSet & paths) {
	NOT_IMP; return PathSet(); };

/* Query substitute info (i.e. references, derivers and download
	 sizes) of a set of paths.	If a path does not have substitute
	 info, it's omitted from the resulting ‘infos’ map. */
void nix::Store_client::querySubstitutablePathInfos(const PathSet & paths,
	                                                SubstitutablePathInfos & infos) {
	NOT_IMP; };

/**
 * Copy the contents of a path to the store and register the
 * validity the resulting path.	The resulting path is returned.
 * The function object `filter' can be used to exclude files (see
 * libutil/archive.hh).
 */
Path
Store_client::addToStore(const Path & srcPath,
                         bool recursive, HashType hashAlgo,
                         PathFilter & filter, bool repair)
{
	debug(format("adding ‘%1%’ to the store") % srcPath);

	using namespace Vfs;

	char const *path_str = srcPath.c_str();

	Directory_service::Stat stat = status(srcPath);

	uint8_t buf[Builder::MAX_NAME_LEN];

	string name = srcPath.substr(srcPath.rfind("/")+1, srcPath.size()-1);

	if (stat.mode & Directory_service::STAT_MODE_DIRECTORY) {
		hash_dir(buf, srcPath);
		Store_hash::encode(buf, name.c_str(), sizeof(buf));
		if (_builder.valid((char *) buf))
			return "/" + string((char *) buf);

		name = add_dir(srcPath);

	} else if (stat.mode & Directory_service::STAT_MODE_FILE) {
		hash_file(buf, srcPath);
		Store_hash::encode(buf, name.c_str(), sizeof(buf));
		if (_builder.valid((char *) buf))
			return "/" + string((char *) buf);

		name = add_file(srcPath);
	} else
		throw nix::Error(format("addToStore: `%1%' has an inappropriate file type") % name);

	return "/" + name;
}

/**
 * Like addToStore, but the contents written to the output path is
 * a regular file containing the given string.
 */
Path nix::Store_client::addTextToStore(const string & name, const string & text,
	                                      const PathSet & references, bool repair)
{
	using namespace File_system;

	{
		string object_name = hash_text(name, text);
		if (_builder.valid(object_name.c_str()))
			return object_name;
	}
	{
		debug(format("adding text ‘%1%’ to the store") % name);

		char const *name_str = name.c_str();

		Genode::Allocator_avl fs_block_alloc(Genode::env()->heap());
		Store_import::Connection fs(fs_block_alloc);
		
		File_handle handle;
		try {
			Dir_handle root = fs.dir("/", false);
			Handle_guard root_guard(fs, root);
			handle = fs.file(root, name_str, File_system::WRITE_ONLY, true);
		} catch (...) {
			PERR("opening export handle for ‘%s’", name.c_str());
			throw;
		}
		Handle_guard file_guard(fs, handle);

		size_t count = text.size();
		size_t offset = 0;

		File_system::Session::Tx::Source &source = *fs.tx();
		size_t const max_packet_size =
			Genode::min(source.bulk_buffer_size() / 2, count);

		size_t remaining_count = count;

		while (remaining_count) {
			size_t const curr_packet_size = Genode::min(remaining_count, max_packet_size);

			File_system::Packet_descriptor
				packet(source.alloc_packet(curr_packet_size), 0,
				       handle, File_system::Packet_descriptor::WRITE,
				       curr_packet_size, offset);
			Packet_guard packet_guard(source, packet);

			text.copy((char *)source.packet_content(packet), curr_packet_size, offset);

			source.submit_packet(packet);
			packet = source.get_acked_packet();
			if (!packet.succeeded())
				throw nix::Error(format("addTextToStore: writing `%1%' failed") % name);

			offset          += packet.length();
			remaining_count -= packet.length();
		}

		//foreach (PathSet::const_iterator, i, references)
		//	_store.add_reference(handle, ((nix::Path)*i).c_str());
		return "/" + finalize_import(fs, name_str);
	}
};

		/* Export a store path, that is, create a NAR dump of the store
			 path and append its references and its deriver.	Optionally, a
			 cryptographic signature (created by OpenSSL) of the preceding
			 data is attached. */
		void nix::Store_client::exportPath(const Path & path, bool sign,
				Sink & sink) { NOT_IMP; };

		/* Import a sequence of NAR dumps created by exportPaths() into
			 the Nix store. */
		Paths nix::Store_client::importPaths(bool requireSignature, Source & source) {
			NOT_IMP; return Paths(); };

		/* Ensure that a path is valid.	If it is not currently valid, it
			 may be made valid by running a substitute (if defined for the
			 path). */
		void nix::Store_client::ensurePath(const Path & path) { NOT_IMP; };

		/* Add a store path as a temporary root of the garbage collector.
			 The root disappears as soon as we exit. */
		void nix::Store_client::addTempRoot(const Path & path) { NOT_IMP; };

		/* Add an indirect root, which is merely a symlink to `path' from
			 /nix/var/nix/gcroots/auto/<hash of `path'>.	`path' is supposed
			 to be a symlink to a store path.	The garbage collector will
			 automatically remove the indirect root when it finds that
			 `path' has disappeared. */
		void nix::Store_client::addIndirectRoot(const Path & path) { NOT_IMP; };

		/* Acquire the global GC lock, then immediately release it.	This
			 function must be called after registering a new permanent root,
			 but before exiting.	Otherwise, it is possible that a running
			 garbage collector doesn't see the new root and deletes the
			 stuff we've just built.	By acquiring the lock briefly, we
			 ensure that either:

			 - The collector is already running, and so we block until the
				 collector is finished.	The collector will know about our
				 *temporary* locks, which should include whatever it is we
				 want to register as a permanent lock.

			 - The collector isn't running, or it's just started but hasn't
				 acquired the GC lock yet.	In that case we get and release
				 the lock right away, then exit.	The collector scans the
				 permanent root and sees our's.

			 In either case the permanent root is seen by the collector. */
		void nix::Store_client::syncWithGC() { NOT_IMP; };

		/* Find the roots of the garbage collector.	Each root is a pair
			 (link, storepath) where `link' is the path of the symlink
			 outside of the Nix store that point to `storePath'.	*/
		Roots nix::Store_client::findRoots() { NOT_IMP; return Roots(); };

		/* Perform a garbage collection. */
		void nix::Store_client::collectGarbage(const GCOptions & options, GCResults & results) { NOT_IMP; };

		/* Return the set of paths that have failed to build.*/
		PathSet nix::Store_client::queryFailedPaths() {
			NOT_IMP; return PathSet(); };

		/* Clear the "failed" status of the given paths.	The special
			 value `*' causes all failed paths to be cleared. */
		void nix::Store_client::clearFailedPaths(const PathSet & paths) { NOT_IMP; };

		/* Return a string representing information about the path that
			 can be loaded into the database using `nix-store --load-db' or
			 `nix-store --register-validity'. */
		string nix::Store_client::makeValidityRegistration(const PathSet & paths,
				bool showDerivers, bool showHash) { NOT_IMP; return string(); }

/**
 * Optimise the disk space usage of the Nix store by hard-linking files
 * with the same contents.
  */
void
nix::Store_client::optimiseStore() { NOT_IMP; };
