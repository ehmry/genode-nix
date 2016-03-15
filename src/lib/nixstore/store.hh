/*
 * \brief  Store-api interface to Genode component sessions
 * \author Emery Hemingway
 * \date   2015-05-27
 */

#ifndef __NIXSTORE__STORE_H_
#define __NIXSTORE__STORE_H_

/* Nix includes */
#include <nix/types.h>
#include <util.hh>

/* Genode includes */
#include <libstore/derivations.hh>
#include <libstore/store-api.hh>
#include <builder_session/connection.h>
#include <file_system/util.h>
#include <vfs/file_system.h>
#include <base/allocator_avl.h>
#include <base/lock.h>
#include <os/path.h>

namespace nix {

	class Store;

	Path readStorePath(Source & from);
	template<class T> T readStorePaths(Source & from);

}

/**
 * Stores fulfils nix::StoreAPI but imports files to the store
 * using a file system session and builds with a Builder sessions.
 */
class nix::Store : public nix::StoreAPI
{
	private:

		Builder::Connection  _builder;
		Genode::Lock         _packet_lock;

		void hash_dir(uint8_t *buf, nix::Path const &src_path);
		void hash_file(uint8_t *buf, nix::Path const &src_path);
		void hash_symlink(uint8_t *buf, nix::Path const &src_path);

		void copy_dir(File_system::Session   &fs,
		              File_system::Dir_handle handle,
		              nix::Path const         &src_path,
		              nix::Path const         &dst_path);

		void copy_file(File_system::Session  &fs,
		               File_system::File_handle handle,
		               nix::Path const         &src_path,
		               nix::Path const         &dst_path);

		void copy_symlink(File_system::Session       &fs,
		                  File_system::Symlink_handle symlink_handle,
		                  nix::Path const            &src_path,
		                  nix::Path const            &dst_path);

		string add_file(const nix::Path &path);
		string add_dir(const nix::Path &path);

	public:

		Store() { if (_vfs == nullptr) throw Error("Nix VFS uninitialized"); }

		Builder::Session &builder() { return _builder; }

		/************************
		 ** StoreAPI interface **
		 ************************/

		/* Check whether a path is valid. */ 
		bool isValidPath(const nix::Path & path) override;

		/* Query which of the given paths is valid. */
		PathSet queryValidPaths(const PathSet & paths) override;

		/* Query the set of all valid paths. */
		PathSet queryAllValidPaths() override;

		/* Query information about a valid path. */
		ValidPathInfo queryPathInfo(const nix::Path & path) override;

		/* Query the hash of a valid path. */ 
		Hash queryPathHash(const nix::Path & path) override;

		/* Query the set of outgoing FS references for a store path.	The
			 result is not cleared. */
		void queryReferences(const nix::Path & path,
				PathSet & references) override;

		/* Queries the set of incoming FS references for a store path.
			 The result is not cleared. */
		void queryReferrers(const nix::Path & path,
				PathSet & referrers) override;

		/* Query the deriver of a store path.	Return the empty string if
			 no deriver has been set. */
		nix::Path queryDeriver(const nix::Path & path) override;

		/* Return all currently valid derivations that have `path' as an
			 output.	(Note that the result of `queryDeriver()' is the
			 derivation that was actually used to produce `path', which may
			 not exist anymore.) */
		PathSet queryValidDerivers(const nix::Path & path) override;

		/* Query the outputs of the derivation denoted by `path'. */
		PathSet queryDerivationOutputs(const nix::Path & path) override;

		/* Query the output names of the derivation denoted by `path'. */
		StringSet queryDerivationOutputNames(const nix::Path & path) override;

		/* Query the full store path given the hash part of a valid store
			 path, or "" if the path doesn't exist. */
		nix::Path queryPathFromHashPart(const string & hashPart) override;

		/* Query which of the given paths have substitutes. */
		PathSet querySubstitutablePaths(const PathSet & paths) override;

		/* Query substitute info (i.e. references, derivers and download
			 sizes) of a set of paths.	If a path does not have substitute
			 info, it's omitted from the resulting ‘infos’ map. */
		void querySubstitutablePathInfos(const PathSet & paths,
				SubstitutablePathInfos & infos) override;

		Path addToStore(const string & name, const Path & srcPath,
		                bool recursive = true, HashType hashAlgo = htDEFAULT,
		                PathFilter & filter = defaultPathFilter, bool repair = false) override;

		/**
		 * Like addToStore, but the contents written to the output path is
		 * a regular file containing the given string.
		 */
		Path addTextToStore(const string & name, const string & s,
				const PathSet & references, bool repair = false) override;

		/**
		 * Add raw data to the store.
		 */
		Path addDataToStore(const string & name,
		                    void *buf, size_t len,
		                    bool repair);

		/* Export a store path, that is, create a NAR dump of the store
			 path and append its references and its deriver.	Optionally, a
			 cryptographic signature (created by OpenSSL) of the preceding
			 data is attached. */
		void exportPath(const nix::Path & path, bool sign,
				Sink & sink) override;

		/* Import a sequence of NAR dumps created by exportPaths() into
			 the Nix store. */
		Paths importPaths(bool requireSignature, Source & source) override;

		/* For each path, if it's a derivation, build it.	Building a
			 derivation means ensuring that the output paths are valid.	If
			 they are already valid, this is a no-op.	Otherwise, validity
			 can be reached in two ways.	First, if the output paths is
			 substitutable, then build the path that way.	Second, the
			 output paths can be created by running the builder, after
			 recursively building any sub-derivations. For inputs that are
			 not derivations, substitute them. */
		void buildPaths(const PathSet & paths, BuildMode buildMode = bmNormal) override;

		/* Build a single non-materialized derivation (i.e. not from an
		   on-disk .drv file). Note that ‘drvPath’ is only used for
		   informational purposes. */
		BuildResult buildDerivation(const Path & drvPath, const BasicDerivation & drv,
		                            BuildMode buildMode = bmNormal) override;

		/* Ensure that a path is valid.	If it is not currently valid, it
			 may be made valid by running a substitute (if defined for the
			 path). */
		void ensurePath(const nix::Path & path) override;

		/* Add a store path as a temporary root of the garbage collector.
			 The root disappears as soon as we exit. */
		void addTempRoot(const nix::Path & path) override;

		/* Add an indirect root, which is merely a symlink to `path' from
			 /nix/var/nix/gcroots/auto/<hash of `path'>.	`path' is supposed
			 to be a symlink to a store path.	The garbage collector will
			 automatically remove the indirect root when it finds that
			 `path' has disappeared. */
		void addIndirectRoot(const nix::Path & path) override;

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
		void syncWithGC() override;

		/* Find the roots of the garbage collector.	Each root is a pair
			 (link, storepath) where `link' is the path of the symlink
			 outside of the Nix store that point to `storePath'.	*/
		Roots findRoots() override;

		/* Perform a garbage collection. */
		void collectGarbage(const GCOptions & options, GCResults & results) override;

		/* Return the set of paths that have failed to build.*/
		PathSet queryFailedPaths() override;

		/* Clear the "failed" status of the given paths.	The special
			 value `*' causes all failed paths to be cleared. */
		void clearFailedPaths(const PathSet & paths) override;

		/* Optimise the disk space usage of the Nix store by hard-linking files
			 with the same contents. */
		void optimiseStore() override;

		/* Check the integrity of the Nix store.  Returns true if errors
			remain. */
		bool verifyStore(bool checkContents, bool repair) override;

};

#endif
