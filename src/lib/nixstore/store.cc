/*
 * \brief  Store-api interface to Genode component sessions
 * \author Emery Hemingway
 * \date   2015-05-27
 */

/* Nix includes */
#include "store.hh"
#include <libutil/util.hh>

/* Genode includes */
#include <file_system_session/connection.h>
#include <store_hash/encode.h>
#include <hash/blake2s.h>
#include <os/config.h>
#include <dataspace/client.h>
#include <base/log.h>


#define NOT_IMP Genode::error(__func__, " not implemented")


using namespace nix;

template<typename FUNC>
void try_file_system(FUNC const &func)
{
	using namespace File_system;

	try { func(); }

	catch (Invalid_handle) {
		Genode::error("unhandled Invalid_handle exception"); throw; }
	catch (Invalid_name) {
		Genode::error("unhandled Invalid_name exception"); throw; }
	catch (Lookup_failed) {
		Genode::error("unhandled Lookup_failed exception"); throw; }
	catch (Name_too_long) {
		Genode::error("unhandled Name_too_long exception"); throw; }
	catch (Node_already_exists) {
		Genode::error("unhandled Node_already_exists exception"); throw; }
	catch (No_space) {
		Genode::error("unhandled No_space exception"); throw; }
	catch (Not_empty) {
		Genode::error("unhandled Not_empty exception"); throw; }
	catch (Out_of_metadata) {
		Genode::error("unhandled Out_of_metadata exception"); throw; }
	catch (Permission_denied) {
		Genode::error("unhandled Permission_denied exception"); throw; }
}


Path nix::readStorePath(Source & from)
{
    Path path = readString(from);
    return path;
}


template<class T> T nix::readStorePaths(Source & from)
{
    T paths = readStrings<T>(from);
    return paths;
}

template PathSet nix::readStorePaths(Source & from);


static string hash_text(const string &name, const string &text)
{
	::Hash::Blake2s hash;
	uint8_t         buf[Nix_store::MAX_NAME_LEN];

	hash.update((uint8_t*)text.data(), text.size());
	hash.update((uint8_t *)"\0f\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, sizeof(buf));
	Store_hash::encode(buf, name.c_str(), sizeof(buf));

	return (char *)buf;
}


static nix::Path finalize_ingest(File_system::Session &fs, char const *name)
{
	using namespace File_system;

	Symlink_handle link_handle;
	try {
		Dir_handle root = fs.dir("/", false);
		Handle_guard root_guard(fs, root);
		link_handle = fs.symlink(root, name, true);
	} catch (...) {
		Genode::error("failed to close ingest handle for ", name);
		throw;
	}
	Handle_guard link_guard(fs, link_handle);

	File_system::Session::Tx::Source &source = *fs.tx();

	File_system::Packet_descriptor
		packet(source.alloc_packet(Nix_store::MAX_NAME_LEN),
		       link_handle, File_system::Packet_descriptor::READ,
		       Nix_store::MAX_NAME_LEN, 0);
	Packet_guard packet_guard(source, packet);

	source.submit_packet(packet);
	packet = source.get_acked_packet();

	if (!packet.length())
		throw nix::Error(format("finalising ingest of ‘%1%’") % name);
	return nix::Path(source.packet_content(packet), packet.length());
}


void Store::copy_dir(File_system::Session   &fs,
                            File_system::Dir_handle ingest_dir,
                            nix::Path const        &src_path,
                            nix::Path const        &dst_path)
{
	using namespace Vfs;
	using namespace File_system;

	Directory_service::Dirent dirent;

	/* Use a map so that entries are sorted. */
	std::map<string, unsigned char> entries;

	for (file_offset i = 0;; ++i) {
		_vfs->dirent(src_path.c_str(), i, dirent);
		if (dirent.type == Directory_service::DIRENT_TYPE_END) break;

		entries.insert(
			std::pair<string, Directory_service::Dirent_type>
			(dirent.name, dirent.type));
	}

	for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
		nix::Path subpath = src_path + "/" + i->first;

		nix::Path sub_src_path = src_path + "/" + i->first;
		nix::Path sub_dst_path = dst_path + "/" + i->first;

		switch (i->second) {
		case Directory_service::DIRENT_TYPE_DIRECTORY: {
			File_system::Dir_handle sub_handle;
			try {
				sub_handle = fs.dir(sub_dst_path.c_str(), true);
			} catch (...) {
				Genode::error("error opening ingest directory handle for ", sub_dst_path.c_str());
				throw;
			}
			File_system::Handle_guard sub_guard(fs, sub_handle);

			copy_dir(fs, sub_handle, sub_src_path, sub_dst_path);
			break;
		}
		
		case Directory_service::DIRENT_TYPE_FILE: {
			File_system::File_handle file_handle;
			try {
				file_handle = fs.file(ingest_dir, i->first.c_str(),
				                      File_system::WRITE_ONLY, true);
			} catch (...) {
				Genode::error("error opening ingest file handle for ", sub_dst_path.c_str());
				throw;
			}
			File_system::Handle_guard sub_guard(fs, file_handle);

			copy_file(fs, file_handle, sub_src_path, sub_dst_path);
			break;
		}
		
		case Directory_service::DIRENT_TYPE_SYMLINK: {
			File_system::Symlink_handle link_handle;
			try {
				link_handle = fs.symlink(ingest_dir, i->first.c_str(), true);
			} catch (...) {
				Genode::error("error opening ingest symlink handle for ", sub_dst_path.c_str());
				throw;
			}
			File_system::Handle_guard sub_guard(fs, link_handle);

			copy_symlink(fs, link_handle, sub_src_path, sub_dst_path);
			break;
		}
		
		default:
			Genode::error("skipping irregular file ", sub_src_path.c_str());
		}
	}
}


void Store::copy_file(File_system::Session    &fs,
                             File_system::File_handle file_handle,
                             nix::Path const         &src_path,
                             nix::Path const         &dst_path)
{
	using namespace Vfs;

	Directory_service::Stat stat = status(src_path);
	file_size remaining_count    = stat.size;
	file_size seek_offset        = 0;

	Vfs_handle *vfs_handle = nullptr;
	if (_vfs->open(src_path.c_str(),
	                   Directory_service::OPEN_MODE_RDONLY,
	                   &vfs_handle, *Genode::env()->heap()) != Directory_service::OPEN_OK)
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
		if (!packet.length())
			throw nix::Error(format("writing file ‘%1%’") % dst_path);

		/* prepare next iteration */
		remaining_count -= packet.length();
		if (remaining_count) {
			seek_offset += packet.length();
			vfs_handle->seek(seek_offset);
		}
	}
}


void Store::copy_symlink(File_system::Session       &fs,
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
		       symlink_handle,
		       File_system::Packet_descriptor::WRITE,
		       0, 0);
	File_system::Packet_guard packet_guard(source, packet);

	/* Read from the VFS to a packet. */
	file_size vfs_count;
	if (_vfs->readlink(src_path.c_str(), source.packet_content(packet),
	                            packet.size(), vfs_count) != Directory_service::READLINK_OK)
		throw Error(format("reading symlink ‘%1%’") % src_path);
	PDBG("%s readlink count %llu", src_path.c_str(), vfs_count);
	if (vfs_count) {
		packet.length(vfs_count);

		/* pass packet to server side */
		source.submit_packet(packet);

		packet = source.get_acked_packet();
		if (!packet.length())
			throw Error(format("copying symlink ‘%1%’ to `%2%'") % src_path % dst_path);
	}
}


void
Store::hash_file(uint8_t *buf, const string &name, nix::Path const &src_path)
{
	using namespace Vfs;

	::Hash::Blake2s hash;
	file_size       remaining;
	file_size       pos = 0;

	Directory_service::Stat stat = status(src_path);

	uint8_t data[Genode::min(4096, stat.size)];

	Vfs_handle *vfs_handle = nullptr;
	if (_vfs->open(src_path.c_str(),
	                        Directory_service::OPEN_MODE_RDONLY,
	                        &vfs_handle, *Genode::env()->heap()) != Directory_service::OPEN_OK)
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

	hash.update((uint8_t*)"\0f\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}

void
Store::hash_symlink(uint8_t *buf, const string &name, nix::Path const &src_path)
{
	using namespace Vfs;

	::Hash::Blake2s hash;
	uint8_t         data[File_system::MAX_PATH_LEN];

	file_size vfs_count;
	if (_vfs->readlink(src_path.c_str(), (char *)data,
	                            sizeof(data), vfs_count) != Directory_service::READLINK_OK)
		throw Error(format("reading symlink ‘%1%’") % src_path);

	PDBG("%s readlink count %llu", src_path.c_str(), vfs_count);
	hash.update(data, vfs_count);

	hash.update((uint8_t*)"\0s\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}

string
Store::add_file(const string &name, nix::Path const &src_path)
{
	using namespace Vfs;

	Directory_service::Stat stat = status(src_path);
	file_size remaining = stat.size;;
	file_size offset    = 0;

	Vfs_handle *vfs_handle = nullptr;
	if (_vfs->open(src_path.c_str(),
	               Directory_service::OPEN_MODE_RDONLY,
	               &vfs_handle, *Genode::env()->heap()) != Directory_service::OPEN_OK)
		throw Error(format("getting handle on file ‘%1%’") % src_path);
	Vfs_handle::Guard vfs_guard(vfs_handle);

	File_system::Connection fs(_env, _fs_tx_alloc, "ingest");

	File_system::File_handle ingest_handle;
	try_file_system([&] {
		try {
			File_system::Dir_handle root_handle = fs.dir("/", false);
			File_system::Handle_guard root_guard(fs, root_handle);

			ingest_handle = fs.file(root_handle, name.c_str(),
			                        File_system::WRITE_ONLY, true);
		} catch (...) {
			Genode::error("error opening file handle at ingest session for ", name.c_str());
			throw;
		}
	});
	File_system::Handle_guard fs_guard(fs, ingest_handle);

	File_system::Session::Tx::Source &source = *fs.tx();
	size_t const max_packet_size = source.bulk_buffer_size() / 2;

	while (remaining) {
		vfs_handle->seek(offset);
		size_t const curr_packet_size = Genode::min(remaining, max_packet_size);

		File_system::Packet_descriptor
			packet(source.alloc_packet(curr_packet_size),
			       ingest_handle, File_system::Packet_descriptor::WRITE,
			       0, offset);
		File_system::Packet_guard guard(source, packet);

		file_size vfs_count;
		if (vfs_handle->fs().read(vfs_handle, source.packet_content(packet),
		                          curr_packet_size, vfs_count) != File_io_service::READ_OK)
			throw Error(format("reading file ‘%1%’") % src_path);

		packet.length(vfs_count);

		source.submit_packet(packet);
		packet = source.get_acked_packet();
		if (!packet.length())
			throw nix::Error(format("addPathToStore: writing `%1%' failed") % src_path);
		remaining -= packet.length();
		offset    += packet.length();
	}

	return finalize_ingest(fs, name.c_str());
}


void
Store::hash_dir(uint8_t *buf, const string &name, nix::Path const &src_path)
{
	using namespace Vfs;

	::Hash::Blake2s           hash;
	Directory_service::Dirent dirent;

	/* Use a map so that entries are sorted. */
	std::map<string, unsigned char> entries;

	for (file_offset i = 0;; ++i) {
		_vfs->dirent(src_path.c_str(), i, dirent);
		if (dirent.type == Directory_service::DIRENT_TYPE_END) break;

		entries.insert(
			std::pair<string, Directory_service::Dirent_type>
			(dirent.name, dirent.type));
	}

	for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
		nix::Path subpath = src_path + "/" + i->first;

		if (i->second == Directory_service::DIRENT_TYPE_DIRECTORY) {
			hash_dir(buf, i->first, subpath);
			hash.update(buf, hash.size());

		} else if (i->second == Directory_service::DIRENT_TYPE_FILE) {
			hash_file(buf, i->first, subpath);
			hash.update(buf, hash.size());

		} else if (i->second == Directory_service::DIRENT_TYPE_SYMLINK) {
			hash_symlink(buf, i->first, subpath);
			hash.update(buf, hash.size());

		} else {
			Genode::error("unhandled file type for ", subpath.c_str());
		}
	}

	hash.update((uint8_t*)"\0d\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}


string
Store::add_dir(const string &name, nix::Path const &src_path)
{
	using namespace File_system;

	File_system::Connection fs(_env, _fs_tx_alloc, "ingest");
	nix::Path const dst_path = "/" + name;

	try_file_system([&] {
		File_system::Dir_handle ingest_dir;
		try { ingest_dir = fs.dir(dst_path.c_str(), true); }
		catch (...) {
			Genode::error("opening ingest directory handle for ", name.c_str());
			throw;
		}
		File_system::Handle_guard dir_guard(fs, ingest_dir);

		copy_dir(fs, ingest_dir, src_path, dst_path);
	});

	return finalize_ingest(fs, name.c_str());
}


/************************
 ** StoreAPI interface **
************************/

/**
 * Check whether a path is valid.
 */
bool
nix::Store::isValidPath(const nix::Path & path)
{
	// slash hack
	char const *_path = path.c_str();
	while (*_path == '/') ++_path;

	return _store_session.dereference(_path) != "";
}


/* Query which of the given paths is valid. */
PathSet
Store::queryValidPaths(const PathSet & paths) {
			NOT_IMP; return PathSet(); };

/* Query the set of all valid paths. */
PathSet nix::Store::queryAllValidPaths() {
	NOT_IMP; return PathSet(); };

/* Query information about a valid path. */
ValidPathInfo nix::Store::queryPathInfo(const Path & path) {
	NOT_IMP; return ValidPathInfo(); };

/* Query the hash of a valid path. */
nix::Hash nix::Store::queryPathHash(const Path & path) {
	NOT_IMP; return Hash(); };

/* Query the set of outgoing FS references for a store path.	The
	 result is not cleared. */
void nix::Store::queryReferences(const nix::Path & path,
		PathSet & references)
{
	if (!isValidPath(path))
		throw Error(format("path ‘%1%’ is not valid") % path);
};

	/* Queries the set of incoming FS references for a store path.
	 The result is not cleared. */
void nix::Store::queryReferrers(const nix::Path & path,
		PathSet & referrers) { NOT_IMP; };

/* Query the deriver of a store path.	Return the empty string if
	 no deriver has been set. */
nix::Path nix::Store::queryDeriver(const Path & path) {
	NOT_IMP; return nix::Path(); };

/* Return all currently valid derivations that have `path' as an
	 output.	(Note that the result of `queryDeriver()' is the
	 derivation that was actually used to produce `path', which may
	 not exist anymore.) */
PathSet nix::Store::queryValidDerivers(const Path & path) {
	NOT_IMP; return PathSet(); };

/* Query the outputs of the derivation denoted by `path'. */
PathSet nix::Store::queryDerivationOutputs(const Path & path) {
	NOT_IMP; return PathSet(); };

/* Query the output names of the derivation denoted by `path'. */
StringSet nix::Store::queryDerivationOutputNames(const Path & path) {
	NOT_IMP; return StringSet(); };

/* Query the full store path given the hash part of a valid store
	 path, or "" if the path doesn't exist. */
nix::Path nix::Store::queryPathFromHashPart(const string & hashPart) {
	NOT_IMP; return Path(); };

/* Query which of the given paths have substitutes. */
PathSet nix::Store::querySubstitutablePaths(const PathSet & paths) {
	NOT_IMP; return PathSet(); };

/* Query substitute info (i.e. references, derivers and download
	 sizes) of a set of paths.	If a path does not have substitute
	 info, it's omitted from the resulting ‘infos’ map. */
void nix::Store::querySubstitutablePathInfos(const PathSet & paths,
	                                                SubstitutablePathInfos & infos) {
	NOT_IMP; };

/**
 * Copy the contents of a path to the store and register the
 * validity the resulting path.	The resulting path is returned.
 * The function object `filter' can be used to exclude files (see
 * libutil/archive.hh).
 */
nix::Path
Store::addToStore(const string &name, const nix::Path &path,
                  bool recursive, HashType hashAlgo,
                  PathFilter & filter, bool repair)
{
	debug(format("adding ‘%1%’ to the store") % name);

	using namespace Vfs;

	nix::Path const srcPath = canonPath(path, true);
	char const *path_str = srcPath.c_str();

	Directory_service::Stat stat = status(srcPath);

	uint8_t buf[Nix_store::MAX_NAME_LEN];

	string final_name;

	if (stat.directory()) {
		hash_dir(buf, name, srcPath);
		Store_hash::encode(buf, name.c_str(), sizeof(buf));
		if (_store_session.dereference((char *) buf) != "") {
			return "/" + string((char *) buf);
		}
		final_name = add_dir(name, srcPath);

	} else if (stat.regular()) {
		hash_file(buf, name, srcPath);
		Store_hash::encode(buf, name.c_str(), sizeof(buf));
		if (_store_session.dereference((char *) buf) != "") {
			return "/" + string((char *) buf);
		}

		final_name = add_file(name, srcPath);
	} else
		throw nix::Error(format("addToStore: `%1%' has an inappropriate file type") % srcPath);

	if (final_name.compare((char *)buf))
		throw nix::Error(format("addToStore: %1% hashed locally to '%2%' but ingest reports `%3%' ") % name % buf % final_name);

	return "/" + final_name;
}

/**
 * Like addToStore, but the contents written to the output path is
 * a regular file containing the given string.
 */
nix::Path nix::Store::addTextToStore(const string & name, const string & text,
	                                      const PathSet & references, bool repair)
{
	using namespace File_system;

	string hashed_name = hash_text(name, text);
	if (_store_session.dereference(hashed_name.c_str()) != "")
		return hashed_name;

	{
		debug(format("adding text ‘%1%’ to the store") % name);

		char const *name_str = name.c_str();
		size_t remaining = text.size();
		size_t offset = 0;

		File_system::Connection fs(_env, _fs_tx_alloc, "ingest");
		
		File_handle handle;
		try {
			Dir_handle root = fs.dir("/", false);
			Handle_guard root_guard(fs, root);
			handle = fs.file(root, name_str, File_system::WRITE_ONLY, true);
		} catch (...) {
			Genode::error("error opening handle at ingest session for text ", name.c_str());
			throw;
		}
		Handle_guard file_guard(fs, handle);
		fs.truncate(handle, remaining);


		File_system::Session::Tx::Source &source = *fs.tx();
		size_t const max_packet_size = source.bulk_buffer_size() / 2;

		while (remaining) {
			size_t const curr_packet_size = Genode::min(remaining, max_packet_size);

			File_system::Packet_descriptor
				packet(source.alloc_packet(curr_packet_size),
				       handle, File_system::Packet_descriptor::WRITE,
				       curr_packet_size, offset);
			Packet_guard packet_guard(source, packet);

			text.copy((char *)source.packet_content(packet), curr_packet_size, offset);

			source.submit_packet(packet);
			packet = source.get_acked_packet();
			if (!packet.length())
				throw nix::Error(format("addTextToStore: writing `%1%' failed") % name);

			offset    += packet.length();
			remaining -= packet.length();
		}


		nix::Path final_name = finalize_ingest(fs, name_str);
		if (final_name != hashed_name)
			throw nix::Error(format("addTextToStore: %1% hashed locally to '%2%' but ingest reports `%3%' ") % name % hashed_name % final_name);

		return "/" + final_name;
	}
};


nix::Path nix::Store::addDataToStore(const string & name,
                                       void *buf, size_t len,
                                       bool repair)
{
	using namespace File_system;

	char *p = (char *)buf;
	size_t offset = 0;

	::Hash::Blake2s hash;
	uint8_t path_buf[Genode::max(size_t(Nix_store::MAX_NAME_LEN), hash.size())];

	hash.update((uint8_t*)buf, len);
	hash.update((uint8_t*)"\0f\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(path_buf, sizeof(path_buf));
	Store_hash::encode(path_buf, name.c_str(), sizeof(path_buf));
	if (_store_session.dereference((char *)path_buf) != "")
		return nix::Path((char *)path_buf);
	{
		debug(format("adding dataspace ‘%1%’ to the store") % name);

		char const *name_str = name.c_str();

		File_system::Connection fs(_env, _fs_tx_alloc, "ingest");
		
		File_handle handle;
		try {
			Dir_handle root = fs.dir("/", false);
			Handle_guard root_guard(fs, root);
			handle = fs.file(root, name_str, File_system::WRITE_ONLY, true);
		} catch (...) {
			Genode::error("error opening handle  at ingest session for data ", name.c_str());
			throw;
		}
		Handle_guard file_guard(fs, handle);
		fs.truncate(handle, len);

		File_system::Session::Tx::Source &source = *fs.tx();
		size_t const max_packet_size = source.bulk_buffer_size() / 2;

		while (len) {
			size_t const curr_packet_size = Genode::min(len, max_packet_size);

			File_system::Packet_descriptor
				packet(source.alloc_packet(curr_packet_size),
				       handle, File_system::Packet_descriptor::WRITE,
				       curr_packet_size, offset);
			Packet_guard packet_guard(source, packet);

			Genode::memcpy(source.packet_content(packet), p+offset,
			               curr_packet_size);

			source.submit_packet(packet);
			packet = source.get_acked_packet();
			if (!packet.length())
				throw nix::Error(format("addTextToStore: writing `%1%' failed") % name);

			offset += packet.length();
			len    -= packet.length();
		}

		nix::Path final_name = finalize_ingest(fs, name_str);

		if (final_name.compare((char *)path_buf))
			throw nix::Error(format("addDataToStore: %1% hashed locally to '%2%' but ingest reports `%3%' ") % name % (char *)path_buf % final_name);

		return "/" + final_name;
	}
};


		/* Export a store path, that is, create a NAR dump of the store
			 path and append its references and its deriver.	Optionally, a
			 cryptographic signature (created by OpenSSL) of the preceding
			 data is attached. */
		void nix::Store::exportPath(const nix::Path & path, bool sign,
				Sink & sink) { NOT_IMP; };

		/* Import a sequence of NAR dumps created by exportPaths() into
			 the Nix store. */
		Paths nix::Store::importPaths(bool requireSignature, Source & source) {
			NOT_IMP; return Paths(); };

		/* Add a store path as a temporary root of the garbage collector.
			 The root disappears as soon as we exit. */
		void nix::Store::addTempRoot(const nix::Path & path) { NOT_IMP; };

		/* Add an indirect root, which is merely a symlink to `path' from
			 /nix/var/nix/gcroots/auto/<hash of `path'>.	`path' is supposed
			 to be a symlink to a store path.	The garbage collector will
			 automatically remove the indirect root when it finds that
			 `path' has disappeared. */
		void nix::Store::addIndirectRoot(const nix::Path & path) { NOT_IMP; };

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
		void nix::Store::syncWithGC() { NOT_IMP; };

		/* Find the roots of the garbage collector.	Each root is a pair
			 (link, storepath) where `link' is the path of the symlink
			 outside of the Nix store that point to `storePath'.	*/
		Roots nix::Store::findRoots() { NOT_IMP; return Roots(); };

		/* Perform a garbage collection. */
		void nix::Store::collectGarbage(const GCOptions & options, GCResults & results) { NOT_IMP; };

		/* Return the set of paths that have failed to build.*/
		PathSet nix::Store::queryFailedPaths() {
			NOT_IMP; return PathSet(); };

		/* Clear the "failed" status of the given paths.	The special
			 value `*' causes all failed paths to be cleared. */
		void nix::Store::clearFailedPaths(const PathSet & paths) { NOT_IMP; };

/**
 * Optimise the disk space usage of the Nix store by hard-linking files
 * with the same contents.
  */
void nix::Store::optimiseStore() { NOT_IMP; };

bool nix::Store::verifyStore(bool checkContents, bool repair) { NOT_IMP; return false; }