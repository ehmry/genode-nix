/*
 * \brief  Nix Store-api to Genode sessions glue
 * \author Emery Hemingway
 * \date   2015-05-27
 */

/* Upstream Nix includes. */
#include "nichts_store.h"
#include <libutil/util.hh>

/* Libc includes. */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

/* Genode includes. */
#include <base/printf.h>
#include <hash/blake2s.h>
#include <store_hash/encode.h>


#define NOT_IMP PERR("%s not implemented", __func__)


using namespace nix;


static string hash_text(const string &name, const string &text)
{
	Hash::Blake2s hash;
	uint8_t buf[Builder::MAX_NAME_LEN];

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

	Dir_handle root = fs.dir("/", false);
	Handle_guard root_guard(fs, root);

	Symlink_handle link_handle = fs.symlink(root, name, true);
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
		throw SysError(format("finalising import ‘%1%’") % name);
	return nix::Path(source.packet_content(packet), packet.length());
}


void nix::copy_dir(File_system::Session   &fs,
                   int                     fd,
                   File_system::Dir_handle dir_handle,
                   nix::Path const        &src_path,
                   nix::Path const        &dst_path)
{
	DIR *c_dir = fdopendir(fd);

	for (;;) {
		struct dirent *dirent = readdir(c_dir);
		if (!dirent) {
			break;
		}

		Path sub_src_path = src_path + "/";
		sub_src_path.append(dirent->d_name);

		Path sub_dst_path = dst_path + "/";
		sub_dst_path.append(dirent->d_name);

		switch (dirent->d_type) {
		case DT_DIR:
		{
			File_system::Dir_handle sub_handle = fs.dir(sub_dst_path.c_str(), true);
			File_system::Handle_guard sub_guard(fs, sub_handle);

			AutoCloseFD subfd = open(sub_src_path.c_str(), O_RDONLY | O_DIRECTORY);
			copy_dir(fs, (int)subfd, sub_handle, sub_src_path, sub_dst_path);
			break;
		}

		case DT_LNK:
		{
			// TODO
			PERR("%s does not handle symlinks, skipping %s", sub_src_path.c_str());
			break;
		}

		case DT_REG:
		{
			AutoCloseFD subfd = open(sub_src_path.c_str(), O_RDONLY);

			File_system::File_handle file_handle =
				fs.file(dir_handle, dirent->d_name, File_system::WRITE_ONLY, true);
			File_system::Handle_guard sub_guard(fs, file_handle);
			copy_file(fs, subfd, file_handle, sub_dst_path);
			break;
		}
		default:
			PERR("unhandled file type for %s", sub_src_path.c_str());
			break;
		}
	}
}


void nix::copy_file(File_system::Session    &fs,
                    int                      fd,
                    File_system::File_handle file_handle,
                    Path const              &dst_path)
{
	struct stat sb;
	if (fstat(fd, &sb))
		throw SysError(format("getting size of file ‘%1%’") % dst_path);
	size_t remaining_count = sb.st_size;
	size_t seek_offset = 0;
	bool success = true;

	/* Preallocate the file space. */
	fs.truncate(file_handle, remaining_count);

	File_system::Session::Tx::Source &source = *fs.tx();
	size_t const max_packet_size = source.bulk_buffer_size() / 2;

	while (remaining_count && success) {
		collect_acknowledgements(source);

		size_t const curr_packet_size = std::min(remaining_count, max_packet_size);

		File_system::Packet_descriptor
			packet(source.alloc_packet(curr_packet_size),
			       0,
			       file_handle,
			       File_system::Packet_descriptor::WRITE,
			       curr_packet_size,
			       seek_offset);
		File_system::Packet_guard packet_guard(source, packet);

		/* Read from the libc VFS to a packet. */
		ssize_t n = ::read(fd, source.packet_content(packet), curr_packet_size);
		if (n <= 0) {
			success = false;
		}

		if (success) {
			/* pass packet to server side */
			source.submit_packet(packet);

			packet = source.get_acked_packet();
			success = packet.succeeded();

			/* prepare next iteration */
			seek_offset += n;
			remaining_count -= n;
		}
	}

	if (!success)
		throw SysError(format("reading file ‘%1%’") % dst_path);
}


void
hash_file(uint8_t *buf, nix::Path const &src_path, int fd)
{
	Hash::Blake2s hash;

	struct stat st;
	if (fstat(fd, &st))
		throw SysError(format("getting size of file ‘%1%’") % src_path);
	size_t count = st.st_size;
	size_t remaining_count = count;

	uint8_t data[Genode::min(4096, count)];

	while (count) {
		size_t n = ::read(fd, data, sizeof(data));
		if (n == -1)
			throw SysError(format("reading file ‘%1%’") % src_path);

		if (n == 0)
			break;

		hash.update(data, n);

		count -= n;
	}

	string name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);

	hash.update((uint8_t*)"\0f\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}


string
Store_client::add_file(nix::Path const &src_path, int fd)
{
	struct stat st;
	if (fstat(fd, &st))
		throw SysError(format("getting size of file ‘%1%’") % src_path);
	size_t count = st.st_size;

	Genode::Allocator_avl fs_block_alloc(Genode::env()->heap());
	File_system::Connection fs(fs_block_alloc, 128*1024, "import");

	nix::Path name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);
	File_system::Dir_handle root = fs.dir("/", false);
	File_system::Handle_guard root_guard(fs, root);

	File_system::File_handle handle = fs.file(root, name.c_str(), File_system::WRITE_ONLY, true);
	File_system::Handle_guard file_guard(fs, handle);

	File_system::Session::Tx::Source &source = *fs.tx();
	size_t const max_packet_size = source.bulk_buffer_size() / 2;

	size_t remaining_count = count;
	File_system::seek_off_t offset = 0;

	while (remaining_count) {
		size_t const curr_packet_size = Genode::min(remaining_count, max_packet_size);

		File_system::Packet_descriptor
			packet(source.alloc_packet(curr_packet_size), 0,
			       handle, File_system::Packet_descriptor::WRITE,
			       curr_packet_size, offset);
		File_system::Packet_guard guard(source, packet);

		ssize_t n = ::read(fd, source.packet_content(packet), curr_packet_size);
		if (n == -1)
			throw SysError(format("reading file ‘%1%’") % src_path);

		if (n == 0)
			break;
		packet.length(n);

		source.submit_packet(packet);
		packet= source.get_acked_packet();
		if (!packet.succeeded())
			throw nix::Error(format("addPathToStore: writing `%1%' failed") % src_path);

		remaining_count -= packet.length();
		offset          += packet.length();
	}

	return finalize_import(fs, name.c_str());
}


void
hash_dir(uint8_t *buf, nix::Path const &src_path, int fd)
{
	Hash::Blake2s hash;

	DIR *c_dir = fdopendir(fd);

	std::map<string, unsigned char> entries;

	for (struct dirent *dirent = readdir(c_dir); dirent; dirent = readdir(c_dir)) {
		entries.insert(std::pair<string, unsigned char>(dirent->d_name, dirent->d_type));
	}

	for (auto i = entries.cbegin(); i != entries.cend(); ++i) {
		Path subpath = src_path + "/" + i->first;

		switch (i->second) {
		case DT_DIR: {
			AutoCloseFD subfd = open(subpath.c_str(), O_RDONLY | O_DIRECTORY);
			hash_dir(buf, subpath, subfd);
			hash.update(buf, hash.size());
			break;
		}

		case DT_LNK:
			// TODO
			PERR("%s does not handle symlinks, skipping %s", subpath.c_str());
			break;

		case DT_REG: {
			AutoCloseFD subfd = open(subpath.c_str(), O_RDONLY);
			hash_file(buf, subpath, subfd);
			hash.update(buf, hash.size());
			break;
		}

		default:
			PERR("unhandled file type for %s", subpath.c_str());
			break;
		}
	}

	string name = src_path.substr(src_path.rfind("/")+1, src_path.size()-1);

	hash.update((uint8_t*)"\0d\0", 3);
	hash.update((uint8_t*)name.data(), name.size());

	hash.digest(buf, hash.size());
}


string
Store_client::add_dir(nix::Path const &src_path, int fd)
{
	Genode::Allocator_avl fs_block_alloc(Genode::env()->heap());
	File_system::Connection fs(fs_block_alloc, 128*1024, "import");

	/* The index of the begining of the last path element. */
	int path_offset = src_path.rfind("/");
	Path dst_path = src_path.substr(path_offset, src_path.size());

	File_system::Dir_handle dir = fs.dir(dst_path.c_str(), true);
	File_system::Handle_guard dir_guard(fs, dir);

	copy_dir(fs, (int) fd, dir, src_path, dst_path);

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
	/*
	 * Access the file using libc to ensure
	 * the VFS is used for path resolution.
	 */
	AutoCloseFD fd = open(srcPath.c_str(), O_RDONLY);
	if (fd == -1)
		throw SysError(format("opening file ‘%1%’") % srcPath);
 
	struct stat sb;
	if (fstat(fd, &sb) != 0)
		throw SysError(format("stating file '%1%'") % srcPath);

	uint8_t buf[Builder::MAX_NAME_LEN];

	string name = srcPath.substr(srcPath.rfind("/")+1, srcPath.size()-1);

	if (S_ISDIR(sb.st_mode)) {
		hash_dir(buf, srcPath, fd);
		Store_hash::encode(buf, name.c_str(), sizeof(buf));
		if (_builder.valid((char *) buf)) {
			PINF("%s", (char*)buf);
			return "/" + string((char *) buf);
		}

		name = add_dir(srcPath, fd);
	} else {
		hash_file(buf, srcPath, fd);
		Store_hash::encode(buf, name.c_str(), sizeof(buf));
		if (_builder.valid((char *) buf)) {
			PINF("%s already in the store", (char*)buf);
			return "/" + string((char *) buf);
		}

		lseek(fd, 0, SEEK_SET);
		name = add_file(srcPath, fd);
	}
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
		File_system::Connection fs(fs_block_alloc, 128*1024, "import");

		Dir_handle root = fs.dir("/", false);
		Handle_guard root_guard(fs, root);
		File_handle handle = fs.file(root, name_str, File_system::WRITE_ONLY, true);
		Handle_guard file_guard(fs, handle);

		size_t count = text.size();
		size_t offset = 0;

		File_system::Session::Tx::Source &source = *fs.tx();
		size_t const max_packet_size = source.bulk_buffer_size() / 2;

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

			offset += packet.length();
			remaining_count   -= packet.length();
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
