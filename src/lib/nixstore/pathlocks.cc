#include "pathlocks.hh"
#include "util.hh"

namespace nix {


int openLockFile(const Path & path, bool create)
{
    return -1;
}


void deleteLockFile(const Path & path, int fd) { }


bool lockFile(int fd, LockType lockType, bool wait)
{
    return false;
}


/* This enables us to check whether are not already holding a lock on
   a file ourselves.  POSIX locks (fcntl) suck in this respect: if we
   close a descriptor, the previous lock will be closed as well.  And
   there is no way to query whether we already have a lock (F_GETLK
   only works on locks held by other processes). */
static StringSet lockedPaths; /* !!! not thread-safe */


PathLocks::PathLocks()
    : deletePaths(false)
{
}


PathLocks::PathLocks(const PathSet & paths, const string & waitMsg)
    : deletePaths(false)
{ }


bool PathLocks::lockPaths(const PathSet & _paths,
    const string & waitMsg, bool wait)
{
	return false;
}


PathLocks::~PathLocks()
{
}


void PathLocks::unlock()
{
}


void PathLocks::setDeletion(bool deletePaths)
{
    this->deletePaths = deletePaths;
}


bool pathIsLockedByMe(const Path & path)
{
    return false;
}

 
}
