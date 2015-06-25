
#pragma once

/* Genode includes */
#include <base/allocator_avl.h>
#include <file_system_session/connection.h>

/* Stdcxx includes */
#include <string>
#include <list>
#include <map>
#include <unordered_set>

/* Nix native includes */
#include <nichts_store_session/nichts_store_session.h>
#include "pathlocks.hh"

/* Nix upstream includes */
#include <libstore/store-api.hh>
#include <libutil/hash.hh>
#include <libutil/util.hh>


class sqlite3;
class sqlite3_stmt;


namespace nix {

extern string drvsLogDir;


typedef std::map<Path, Path> Roots;


struct Derivation;


struct OptimiseStats
{
    unsigned long filesLinked;
    unsigned long long bytesFreed;
    unsigned long long blocksFreed;
    OptimiseStats()
    {
        filesLinked = 0;
        bytesFreed = blocksFreed = 0;
    }
};


struct RunningSubstituter
{
    Path program;
    AutoCloseFD to, from, error;
    bool disabled;
    RunningSubstituter() : disabled(false) { };
};


typedef std::map<Path, SubstitutablePathInfo> SubstitutablePathInfos;


typedef list<ValidPathInfo> ValidPathInfos;


/* Wrapper object to close the SQLite database automatically. */
struct SQLite
{
    sqlite3 * db;
    SQLite() { db = 0; }
    ~SQLite();
    operator sqlite3 * () { return db; }
};


/* Wrapper object to create and destroy SQLite prepared statements. */
struct SQLiteStmt
{
    sqlite3 * db;
    sqlite3_stmt * stmt;
    unsigned int curArg;
    SQLiteStmt() { stmt = 0; }
    void create(sqlite3 * db, const string & s);
    void reset();
    ~SQLiteStmt();
    operator sqlite3_stmt * () { return stmt; }
    void bind(const string & value);
    void bind(int value);
    void bind64(long long value);
    void bind();
};


class Store : public StoreAPI
{

	private:

		Genode::Allocator_avl    _fs_block_alloc;
		File_system::Connection  _fs;

		void create_dir(const char *path);

		/**
		 * Open a directory, creating missing parents if specified.
		 */
		File_system::Dir_handle dir_of(const char *path, bool create = false);

		/**
		 * Create a symlink using an atomic operation.
		 */
		void create_symlink(const char *link, const char *target);

		bool path_exists(char const *path);
		bool path_exists(std::string const &path);

		std::string read_file(char const *path);
		std::string read_file(std::string const &path);

		void write_file(Path &path, std::string);
		void write_file(const char *path, std::string);

		void delete_path(char const *path);
		void delete_path(std::string const &path);

		Path readStorePath(Source & from);
		PathSet readStorePaths(Source & from);

private:
    typedef std::map<Path, RunningSubstituter> RunningSubstituters;
    RunningSubstituters runningSubstituters;

    Path linksDir;

	public:

		/* Initialise the local store, upgrading the schema if necessary. */
		Store(bool reserve_space = true, bool read_only = false);

   	 	~Store();

		/**
		 * Open and return a file handle that closes itself on destruction.
		 */

		void close_handle(File_system::Node_handle &);

    /* Implementations of abstract store API methods. */

    bool isValidPath(const Path & path);

    PathSet queryValidPaths(const PathSet & paths);

    PathSet queryAllValidPaths();

    ValidPathInfo queryPathInfo(const Path & path);

    Hash queryPathHash(const Path & path);

    void queryReferences(const Path & path, PathSet & references);

    void queryReferrers(const Path & path, PathSet & referrers);

    Path queryDeriver(const Path & path);

    PathSet queryValidDerivers(const Path & path);

    PathSet queryDerivationOutputs(const Path & path);

    StringSet queryDerivationOutputNames(const Path & path);

    Path queryPathFromHashPart(const string & hashPart);

    PathSet querySubstitutablePaths(const PathSet & paths);

    void querySubstitutablePathInfos(const Path & substituter,
        PathSet & paths, SubstitutablePathInfos & infos);

    void querySubstitutablePathInfos(const PathSet & paths,
        SubstitutablePathInfos & infos);

    Path addToStore(const Path & srcPath,
        bool recursive = true, HashType hashAlgo = htSHA256,
        PathFilter & filter = defaultPathFilter, bool repair = false);

    /* Like addToStore(), but the contents of the path are contained
       in `dump', which is either a NAR serialisation (if recursive ==
       true) or simply the contents of a regular file (if recursive ==
       false). */
    Path addToStoreFromDump(const string & dump, const string & name,
        bool recursive = true, HashType hashAlgo = htSHA256, bool repair = false);

    Path addTextToStore(const string & name, const string & s,
        const PathSet & references, bool repair = false);

    void exportPath(const Path & path, bool sign,
        Sink & sink);

    Paths importPaths(bool requireSignature, Source & source);

    void buildPaths(const PathSet & paths, Nichts_store::Mode mode);
    void buildPaths(const PathSet & paths, nix::BuildMode);

	void buildPath(const char *path, Nichts_store::Mode mode);

    void ensurePath(const Path & path);

    void addTempRoot(const Path & path);

    void addIndirectRoot(const Path & path);

    void syncWithGC();

    Roots findRoots();

    void collectGarbage(const GCOptions & options, GCResults & results);

    /* Optimise the disk space usage of the Nix store by hard-linking
       files with the same contents. */
    void optimiseStore(OptimiseStats & stats);

    /* Generic variant of the above method.  */
    void optimiseStore();

    /* Optimise a single store path. */
    void optimisePath(const Path & path);

    /* Check the integrity of the Nix store.  Returns true if errors
       remain. */
    bool verifyStore(bool checkContents, bool repair);

    /* Register the validity of a path, i.e., that `path' exists, that
       the paths referenced by it exists, and in the case of an output
       path of a derivation, that it has been produced by a successful
       execution of the derivation (or something equivalent).  Also
       register the hash of the file system contents of the path.  The
       hash must be a SHA-256 hash. */
    void registerValidPath(const ValidPathInfo & info);

    void registerValidPaths(const ValidPathInfos & infos);

    /* Register that the build of a derivation with output `path' has
       failed. */
    void registerFailedPath(const Path & path);

    /* Query whether `path' previously failed to build. */
    bool hasPathFailed(const Path & path);

    PathSet queryFailedPaths();

    void clearFailedPaths(const PathSet & paths);

    void vacuumDB();

    /* Repair the contents of the given path by redownloading it using
       a substituter (if available). */
    void repairPath(const Path & path);

    /* Check whether the given valid path exists and has the right
       contents. */
    bool pathContentsGood(const Path & path);

    void markContentsGood(const Path & path);

    void setSubstituterEnv();

	/* Read a derivation, after ensuring its existence through
   	 * ensurePath().
	 */
	Derivation derivationFromPath(const Path & drvPath);

	/* Place in `paths' the set of all store paths in the file system
	 * closure of `storePath'; that is, all paths than can be directly or
	 * indirectly reached from it.  `paths' is not cleared.  If
	 * `flipDirection' is true, the set of paths that can reach
	 * `storePath' is returned; that is, the closures under the
	 * `referrers' relation instead of the `references' relation is
	 * returned. 
	 */
	void computeFSClosure(const Path & path, PathSet & paths,
	                      bool flipDirection = false,
	                      bool includeOutputs = false,
	                      bool includeDerivers = false);

	/* Throw an exception if `path' is not directly in the Nix store. */
	void assertStorePath(const Path & path);

	bool isInStore(const Path & path);
	bool isStorePath(const Path & path);

private:

    Path schemaPath;

    /* Lock file used for upgrading. */
    AutoCloseFD globalLock;

    /* The SQLite database object. */
    SQLite db;

    /* Some precompiled SQLite statements. */
    SQLiteStmt stmtRegisterValidPath;
    SQLiteStmt stmtUpdatePathInfo;
    SQLiteStmt stmtAddReference;
    SQLiteStmt stmtQueryPathInfo;
    SQLiteStmt stmtQueryReferences;
    SQLiteStmt stmtQueryReferrers;
    SQLiteStmt stmtInvalidatePath;
    SQLiteStmt stmtRegisterFailedPath;
    SQLiteStmt stmtHasPathFailed;
    SQLiteStmt stmtQueryFailedPaths;
    SQLiteStmt stmtClearFailedPath;
    SQLiteStmt stmtAddDerivationOutput;
    SQLiteStmt stmtQueryValidDerivers;
    SQLiteStmt stmtQueryDerivationOutputs;
    SQLiteStmt stmtQueryPathFromHashPart;

    /* Cache for pathContentsGood(). */
    std::map<Path, bool> pathContentsGoodCache;

    bool didSetSubstituterEnv;

    /* The file to which we write our temporary roots. */
    Path fnTempRoots;
    AutoCloseFD fdTempRoots;

    int getSchema();

    void openDB(bool create);

    void makeStoreWritable();

    unsigned long long queryValidPathId(const Path & path);

    unsigned long long addValidPath(const ValidPathInfo & info, bool checkOutputs = true);

    void addReference(unsigned long long referrer, unsigned long long reference);

    void appendReferrer(const Path & from, const Path & to, bool lock);

    void rewriteReferrers(const Path & path, bool purge, PathSet referrers);

    void invalidatePath(const Path & path);

    /* Delete a path from the Nix store. */
    void invalidatePathChecked(const Path & path);

    void verifyPath(const Path & path, const PathSet & store,
        PathSet & done, PathSet & validPaths, bool repair, bool & errors);

    void updatePathInfo(const ValidPathInfo & info);

    PathSet queryValidPathsOld();
    ValidPathInfo queryPathInfoOld(const Path & path);

    struct GCState;

    void deleteGarbage(GCState & state, const Path & path);

    void tryToDelete(GCState & state, const Path & path);

    bool canReachRoot(GCState & state, PathSet & visited, const Path & path);

    void deletePathRecursive(GCState & state, const Path & path);

    bool isActiveTempFile(const GCState & state,
        const Path & path, const string & suffix);

    int openGCLock(LockType lockType);

    void removeUnusedLinks(const GCState & state);

    void startSubstituter(const Path & substituter,
        RunningSubstituter & runningSubstituter);

    string getLineFromSubstituter(RunningSubstituter & run);

    template<class T> T getIntLineFromSubstituter(RunningSubstituter & run);

    Path createTempDirInStore();

    Path importPath(bool requireSignature, Source & source);

    void checkDerivationOutputs(const Path & drvPath, const Derivation & drv);

    typedef std::unordered_set<ino_t> InodeHash;

    InodeHash loadInodeHash();
    Strings readDirectoryIgnoringInodes(const Path & path, const InodeHash & inodeHash);
    void optimisePath_(OptimiseStats & stats, const Path & path, InodeHash & inodeHash);

    // Internal versions that are not wrapped in retry_sqlite.
    bool isValidPath_(const Path & path);
    void queryReferrers_(const Path & path, PathSet & referrers);
};


typedef std::pair<dev_t, ino_t> Inode;


/* "Fix", or canonicalise, the meta-data of the files in a store path
   after it has been built.  In particular:
   - the last modification date on each file is set to 1 (i.e.,
     00:00:01 1/1/1970 UTC)
   - the permissions are set of 444 or 555 (i.e., read-only with or
     without execute permission; setuid bits etc. are cleared)
   - the owner and group are set to the Nix user and group, if we're
     running as root. */
void canonicalisePathMetaData(const Path & path, uid_t fromUid);

void canonicaliseTimestampAndPermissions(const Path & path);


MakeError(PathInUse, Error);

struct LocalStore {
	LocalStore(bool);
	void invalidatePathChecked(const Path & path);
};

struct RemoteStore {
	RemoteStore();
};

}
