#include "config.h"

#include "references.hh"
#include "pathlocks.hh"
#include "misc.hh"
#include "globals.hh"
#include "store.hh"
#include "util.hh"
#include "archive.hh"
#include "affinity.hh"
#include "misc.hh"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>

/* Genode includes */
#include <base/signal.h>


namespace nix {

using std::map;


/* Forward definition. */
class Worker;


/* A pointer to a goal. */
class Goal;
typedef std::shared_ptr<Goal> GoalPtr;
typedef std::weak_ptr<Goal> WeakGoalPtr;

struct CompareGoalPtrs {
    bool operator() (const GoalPtr & a, const GoalPtr & b);
};

/* Set of goals. */
typedef set<GoalPtr, CompareGoalPtrs> Goals;
typedef list<WeakGoalPtr> WeakGoals;

/* A map of paths to goals (and the other way around). */
typedef map<Path, WeakGoalPtr> WeakGoalMap;



class Goal : public std::enable_shared_from_this<Goal>
{
public:
    typedef enum {ecBusy, ecSuccess, ecFailed, ecIncompleteClosure} ExitCode;

private:

    /* Backlink to the worker. */
    Worker & worker;

    /* Goals that this goal is waiting for. */
    Goals waitees;

    /* Goals waiting for this one to finish.  Must use weak pointers
       here to prevent cycles. */
    WeakGoals waiters;

    /* Number of goals we are/were waiting for that have failed. */
    unsigned int nrFailed;

    /* Name of this goal for debugging purposes. */
    string name;

    /* Whether the goal is finished. */
    ExitCode exitCode;

    /* The path of the derivation. */
    Path drvPath;

    /* The specific outputs that we need to build.  Empty means all of
       them. */
    StringSet wantedOutputs;

    /* Whether additional wanted outputs have been added. */
    bool needRestart = false;

    /* Whether to retry substituting the outputs after building the
       inputs. */
    bool retrySubstitution = false;

    /* The derivation stored at drvPath. */
    std::unique_ptr<BasicDerivation> drv;

    /* The remainder is state held during the build. */

    /* Locks on the output paths. */
    PathLocks outputLocks;

    /* All input paths (that is, the union of FS closures of the
       immediate input paths). */
    PathSet inputPaths;

    /* Referenceable paths (i.e., input and output paths). */
    PathSet allPaths;

    /* Outputs that are already valid.  If we're repairing, these are
       the outputs that are valid *and* not corrupt. */
    PathSet validPaths;

    /* Outputs that are corrupt or not valid. */
    PathSet missingPaths;

    /* Whether this is a fixed-output derivation. */
    bool fixedOutput;

    typedef void (Goal::*GoalState)();
    GoalState state;

    /* Stuff we need to pass to initChild(). */
    typedef map<Path, Path> DirsInChroot; // maps target path to source path
    typedef map<string, string> Environment;
    Environment env;

    BuildMode buildMode;

    /* If we're repairing without a chroot, there may be outputs that
       are valid but corrupt.  So we redirect these outputs to
       temporary paths. */
    PathSet redirectedBadOutputs;

    BuildResult result;

    /* The current round, if we're building multiple times. */
    unsigned int curRound = 1;

    unsigned int nrRounds;

    /* Path registration info from the previous round, if we're
       building multiple times. Since this contains the hash, it
       allows us to compare whether two rounds produced the same
       result. */
    ValidPathInfos prevInfos;

    Genode::Signal_context sigCtx;

public:

    Goal(const Path & drvPath, const StringSet & wantedOutputs,
        Worker & worker, BuildMode buildMode = bmNormal);
    Goal(const Path & drvPath, const BasicDerivation & drv,
        Worker & worker, BuildMode buildMode = bmNormal);
    ~Goal();

public:

    void addWaitee(GoalPtr waitee);

    virtual void waiteeDone(GoalPtr waitee, ExitCode result);

    void trace(const format & f);

    string getName()
    {
        return name;
    }

    ExitCode getExitCode()
    {
        return exitCode;
    }

    /* Callback in case of a timeout.  It should wake up its waiters,
       get rid of any running child processes that are being monitored
       by the worker (important!), etc. */
    void timedOut();

    string key()
    {
        /* Ensure that derivations get built in order of their name,
           i.e. a derivation named "aardvark" always comes before
           "baboon". And substitution goals always happen before
           derivation goals (due to "b$"). */
        return "b$" + storePathToName(drvPath) + "$" + drvPath;
    }

    void work();

    Path getDrvPath()
    {
        return drvPath;
    }

    /* Add wanted outputs to an already existing derivation goal. */
    void addWantedOutputs(const StringSet & outputs);

    BuildResult getResult() { return result; }

    Genode::Signal_context *context() { return &sigCtx; };

private:

    void amDone(ExitCode result);

    /* The states. */
    void loadDerivation();
    void haveDerivation();
    void outputsSubstituted();
    void closureRepaired();
    void inputsRealised();
    void tryToBuild();
    void buildDone();

    /* Start building a derivation. */
    void startBuilder();

    /* Run the builder's process. */
    void runChild();

    /* Return the set of (in)valid paths. */
    PathSet checkPathValidity(bool returnValid, bool checkHash);

    /* Abort the goal if `path' failed to build. */
    bool pathFailed(const Path & path);

    void done(BuildResult::Status status, const string & msg = "");
};


bool CompareGoalPtrs::operator() (const GoalPtr & a, const GoalPtr & b) {
    string s1 = a->key();
    string s2 = b->key();
    return s1 < s2;
}


/* A mapping used to remember for each child process to what goal it
   belongs, and file descriptors for receiving log data and output
   path creation commands. */
struct Child
{
    WeakGoalPtr goal;
    set<int> fds;
    bool respectTimeouts;
    bool inBuildSlot;
    time_t lastOutput; /* time we last got output on stdout/stderr */
    time_t timeStarted;
};


/* The worker class. */
class Worker
{
private:

    /* Note: the worker should only have strong pointers to the
       top-level goals. */

    /* The top-level goals of the worker. */
    Goals topGoals;

    /* Goals that are ready to do some work. */
    WeakGoals awake;

    /* Goals waiting for a build slot. */
    WeakGoals wantingToBuild;

    /* Number of build slots occupied.  This includes local builds and
       substitutions but not remote builds via the build hook. */
    unsigned int nrLocalBuilds;

    /* Maps used to prevent multiple instantiations of a goal for the
       same derivation / path. */
    WeakGoalMap derivationGoals;

    /* Goals waiting for busy paths to be unlocked. */
    WeakGoals waitingForAnyGoal;

    /* Goals sleeping for a few seconds (polling a lock). */
    WeakGoals waitingForAWhile;

    /* Goals started at the builder */
    WeakGoals builderPending;

    /* Last time the goals in `waitingForAWhile' where woken up. */
    time_t lastWokenUp;\

    Genode::Signal_receiver sigRec;

public:

    /* Set if at least one derivation had a BuildError (i.e. permanent
       failure). */
    bool permanentFailure;

    /* Set if at least one derivation had a timeout. */
    bool timedOut;

    Store & store;

    Worker(Store & store);
    ~Worker();

    /* Make a goal (with caching). */
    GoalPtr makeDerivationGoal(const Path & drvPath, const StringSet & wantedOutputs, BuildMode buildMode = bmNormal);
    std::shared_ptr<Goal> makeBasicDerivationGoal(const Path & drvPath,
        const BasicDerivation & drv, BuildMode buildMode = bmNormal);

    /* Remove a dead goal. */
    void removeGoal(GoalPtr goal);

    /* Wake up a goal (i.e., there is something for it to do). */
    void wakeUp(GoalPtr goal);

    /* Return the number of local build and substitution processes
       currently running (but not remote builds via the build
       hook). */
    unsigned int getNrLocalBuilds();

    /* Put `goal' to sleep until a build slot becomes available (which
       might be right away). */
    void waitForBuildSlot(GoalPtr goal);

    /* Wait for any goal to finish.  Pretty indiscriminate way to
       wait for some resource that some other goal is holding. */
    void waitForAnyGoal(GoalPtr goal);

    /* Wait for a few seconds and then retry this goal.  Used when
       waiting for a lock held by another process.  This kind of
       polling is inefficient, but POSIX doesn't really provide a way
       to wait for multiple locks in the main select() loop. */
    void waitForAWhile(GoalPtr goal);

    /* Loop until the specified top-level goals have finished. */
    void run(const Goals & topGoals);

    unsigned int exitStatus();

    /* Manage a signal context and start a goal at the builder */
    void realize(GoalPtr goal);

};


//////////////////////////////////////////////////////////////////////


void addToWeakGoals(WeakGoals & goals, GoalPtr p)
{
    // FIXME: necessary?
    // FIXME: O(n)
    for (auto & i : goals)
        if (i.lock() == p) return;
    goals.push_back(p);
}


void Goal::addWaitee(GoalPtr waitee)
{
    waitees.insert(waitee);
    addToWeakGoals(waitee->waiters, shared_from_this());
}


void Goal::waiteeDone(GoalPtr waitee, ExitCode result)
{
    assert(waitees.find(waitee) != waitees.end());
    waitees.erase(waitee);

    trace(format("waitee ‘%1%’ done; %2% left") %
        waitee->name % waitees.size());

    if (result == ecFailed || result == ecIncompleteClosure) ++nrFailed;

    if (waitees.empty() || (result == ecFailed && !settings.keepGoing)) {

        /* If we failed and keepGoing is not set, we remove all
           remaining waitees. */
        for (auto & goal : waitees) {
            WeakGoals waiters2;
            for (auto & j : goal->waiters)
                if (j.lock() != shared_from_this()) waiters2.push_back(j);
            goal->waiters = waiters2;
        }
        waitees.clear();

        worker.wakeUp(shared_from_this());
    }
}


void Goal::amDone(ExitCode result)
{
    trace("done");
    assert(exitCode == ecBusy);
    assert(result == ecSuccess || result == ecFailed || result == ecIncompleteClosure);
    exitCode = result;
    for (auto & i : waiters) {
        GoalPtr goal = i.lock();
        if (goal) goal->waiteeDone(shared_from_this(), result);
    }
    waiters.clear();
    worker.removeGoal(shared_from_this());
}


void Goal::trace(const format & f)
{
    debug(format("%1%: %2%") % name % f);
}


//////////////////////////////////////////////////////////////////////


typedef enum {rpAccept, rpDecline, rpPostpone} HookReply;

class SubstitutionGoal;

Goal::Goal(const Path & drvPath, const StringSet & wantedOutputs,
    Worker & worker, BuildMode buildMode)
    : worker(worker)
    , drvPath(drvPath)
    , wantedOutputs(wantedOutputs)
    , buildMode(buildMode)
{
    nrFailed = 0;
    exitCode = ecBusy;
    state = &Goal::loadDerivation;
    name = (format("building of ‘%1%’") % drvPath).str();
    trace("created");
}


Goal::Goal(const Path & drvPath, const BasicDerivation & drv,
    Worker & worker, BuildMode buildMode)
    : worker(worker)
    , drvPath(drvPath)
    , buildMode(buildMode)
{
    nrFailed = 0;
    exitCode = ecBusy;
    this->drv = std::unique_ptr<BasicDerivation>(new BasicDerivation(drv));
    state = &Goal::haveDerivation;
    name = (format("building of %1%") % showPaths(outputPaths(drv))).str();
    trace("created");
}


Goal::~Goal()
{
    trace("goal destroyed");
}


void Goal::timedOut()
{
    if (settings.printBuildTrace)
        printMsg(lvlError, format("@ build-failed %1% - timeout") % drvPath);
    done(BuildResult::TimedOut);
}


void Goal::work()
{
    (this->*state)();
}


void Goal::addWantedOutputs(const StringSet & outputs)
{
    /* If we already want all outputs, there is nothing to do. */
    if (wantedOutputs.empty()) return;

    if (outputs.empty()) {
        wantedOutputs.clear();
        needRestart = true;
    } else
        for (auto & i : outputs)
            if (wantedOutputs.find(i) == wantedOutputs.end()) {
                wantedOutputs.insert(i);
                needRestart = true;
            }
}


void Goal::loadDerivation()
{
    trace("loading derivation");

    assert(worker.store.isValidPath(drvPath));

    /* Get the derivation. */
    drv = std::unique_ptr<BasicDerivation>(new Derivation(derivationFromPath(worker.store, drvPath)));

    haveDerivation();
}


void Goal::haveDerivation()
{
    trace("have derivation");

    /* Check what outputs paths are not already valid. */
    PathSet invalidOutputs = checkPathValidity(false, buildMode == bmRepair);

    /* If they are all valid, then we're done. */
    if (invalidOutputs.size() == 0 && buildMode == bmNormal) {
        done(BuildResult::AlreadyValid);
        return;
    }

    /* Check whether any output previously failed to build.  If so,
       don't bother. */
    for (auto & i : invalidOutputs)
        if (pathFailed(i)) return;

    /* We are first going to try to create the invalid output paths
       through substitutes.  If that doesn't work, we'll build
       them. */

    if (needRestart) {
        needRestart = false;
        haveDerivation();
        return;
    }

    /* Make sure checkPathValidity() from now on checks all
       outputs. */
    wantedOutputs = PathSet();

    /* The inputs must be built before we can build this goal. */
    for (auto & i : dynamic_cast<Derivation *>(drv.get())->inputDrvs)
        addWaitee(worker.makeDerivationGoal(i.first, i.second, buildMode == bmRepair ? bmRepair : bmNormal));

    for (auto & i : drv->inputSrcs) {
        if (worker.store.isValidPath(i)) continue;
        throw Error(format("dependency of ‘%1%’ of ‘%2%’ does not exist") % i % drvPath);
    }

    if (waitees.empty()) /* to prevent hang (no wake-up event) */
        inputsRealised();
    else
        state = &Goal::inputsRealised;
}


void Goal::inputsRealised()
{
    trace("all inputs realised");

    if (nrFailed != 0) {
        printMsg(lvlError,
            format("cannot build derivation ‘%1%’: %2% dependencies couldn't be built")
            % drvPath % nrFailed);
        done(BuildResult::DependencyFailed);
        return;
    }

    if (retrySubstitution) {
        haveDerivation();
        return;
    }

    /* Gather information necessary for computing the closure and/or
       running the build hook. */

    /* The outputs are referenceable paths. */
    for (auto & i : drv->outputs) {
        debug(format("building path ‘%1%’") % i.second.path);
        allPaths.insert(i.second.path);
    }

    /* Determine the full set of input paths. */

    /* First, the input derivations. */
        for (auto & i : dynamic_cast<Derivation *>(drv.get())->inputDrvs) {
            /* Add the relevant output closures of the input derivation
               `i' as input paths.  Only add the closures of output paths
               that are specified as inputs. */
            assert(worker.store.isValidPath(i.first));
            Derivation inDrv = derivationFromPath(worker.store, i.first);
            for (auto & j : i.second)
                if (inDrv.outputs.find(j) != inDrv.outputs.end())
                    computeFSClosure(worker.store, inDrv.outputs[j].path, inputPaths);
                else
                    throw Error(
                        format("derivation ‘%1%’ requires non-existent output ‘%2%’ from input derivation ‘%3%’")
                        % drvPath % j % i.first);
        }

    /* Second, the input sources. */
    for (auto & i : drv->inputSrcs)
        computeFSClosure(worker.store, i, inputPaths);

    debug(format("added input paths %1%") % showPaths(inputPaths));

    allPaths.insert(inputPaths.begin(), inputPaths.end());

    /* Is this a fixed-output derivation? */
    fixedOutput = true;
    for (auto & i : drv->outputs)
        if (i.second.hash == "") fixedOutput = false;

    /* Don't repeat fixed-output derivations since they're already
       verified by their output hash.*/
    nrRounds = fixedOutput ? 1 : settings.get("build-repeat", 0) + 1;

    /* Okay, try to build.  Note that here we don't wait for a build
       slot to become available, since we don't need one if there is a
       build hook. */
    state = &Goal::tryToBuild;
    worker.wakeUp(shared_from_this());
}


static bool canBuildLocally(const BasicDerivation & drv)
{
    return drv.platform == settings.thisSystem || (drv.platform == SYSTEM);
}


static string get(const StringPairs & map, const string & key, const string & def = "")
{
    StringPairs::const_iterator i = map.find(key);
    return i == map.end() ? def : i->second;
}


bool willBuildLocally(const BasicDerivation & drv)
{
    return canBuildLocally(drv);
}


bool substitutesAllowed(const BasicDerivation & drv) { return false; }


void Goal::tryToBuild()
{
    trace("trying to build");

    missingPaths = outputPaths(*drv);
    if (buildMode != bmCheck)
        for (auto & i : validPaths) missingPaths.erase(i);

     /* Okay, we have to build. */
    startBuilder();

    /* This state will be reached when we get a signal from the builder */
    state = &Goal::buildDone;
}


void Goal::buildDone()
{
    trace("build done");

    debug(format("builder process for ‘%1%’ finished") % drvPath);

    for (auto & i : drv->outputs) {
        if (worker.store.store_session().valid(i.second.path.c_str()))
            continue;
        printMsg(lvlError, format("@ build-failed %1%") % drvPath);
        done(BuildResult::PermanentFailure);
        return;
    }

    printMsg(lvlError, format("@ build-succeeded %1% -") % drvPath);
    done(BuildResult::Built);
}


void Goal::startBuilder()
{
    auto f = format(
        buildMode == bmRepair ? "repairing path(s) %1%" :
        buildMode == bmCheck ? "checking path(s) %1%" :
        nrRounds > 1 ? "building path(s) %1% (round %2%/%3%)" :
        "building path(s) %1%");
    f.exceptions(boost::io::all_error_bits ^ boost::io::too_many_args_bit);
    startNest(nest, lvlInfo, f % showPaths(missingPaths) % curRound % nrRounds);

    /* Right platform? */
    if (!canBuildLocally(*drv)) {
        if (settings.printBuildTrace)
            printMsg(lvlError, format("@ unsupported-platform %1% %2%") % drvPath % drv->platform);
        throw Error(
            format("a ‘%1%’ is required to build ‘%3%’, but I am a ‘%2%’")
            % drv->platform % settings.thisSystem % drvPath);
    }

    /* Run the builder. */
    printMsg(lvlChatty, format("executing builder ‘%1%’") % drv->builder);

    worker.realize(shared_from_this());
}


/* Parse a list of reference specifiers.  Each element must either be
   a store path, or the symbolic name of the output of the derivation
   (such as `out'). */
PathSet parseReferenceSpecifiers(const BasicDerivation & drv, string attr)
{
    PathSet result;
    Paths paths = tokenizeString<Paths>(attr);
    for (auto & i : paths) {
        if (isStorePath(i))
            result.insert(i);
        else if (drv.outputs.find(i) != drv.outputs.end())
            result.insert(drv.outputs.find(i)->second.path);
        else throw BuildError(
            format("derivation contains an illegal reference specifier ‘%1%’") % i);
    }
    return result;
}


PathSet Goal::checkPathValidity(bool returnValid, bool checkHash)
{
    PathSet result;
    for (auto & i : drv->outputs) {
        if (!wantOutput(i.first, wantedOutputs)) continue;
        bool good =
            worker.store.isValidPath(i.second.path);
            //(!checkHash || worker.store.pathContentsGood(i.second.path));
        if (good == returnValid) result.insert(i.second.path);
    }
    return result;
}


bool Goal::pathFailed(const Path & path)
{
    if (!settings.cacheFailure) return false;

    printMsg(lvlError, format("builder for ‘%1%’ failed previously (cached)") % path);

    if (settings.printBuildTrace)
        printMsg(lvlError, format("@ build-failed %1% - cached") % drvPath);

    done(BuildResult::CachedFailure);

    return true;
}


void Goal::done(BuildResult::Status status, const string & msg)
{
    result.status = status;
    result.errorMsg = msg;
    amDone(result.success() ? ecSuccess : ecFailed);
    if (result.status == BuildResult::TimedOut)
        worker.timedOut = true;
    if (result.status == BuildResult::PermanentFailure || result.status == BuildResult::CachedFailure)
        worker.permanentFailure = true;
}


//////////////////////////////////////////////////////////////////////


static bool working = false;


Worker::Worker(Store & store)
    : store(store)
{
    /* Debugging: prevent recursive workers. */
    if (working) abort();
    working = true;
    nrLocalBuilds = 0;
    lastWokenUp = 0;
    permanentFailure = false;
    timedOut = false;
}


Worker::~Worker()
{
    working = false;

    /* Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    topGoals.clear();
}


GoalPtr Worker::makeDerivationGoal(const Path & path,
    const StringSet & wantedOutputs, BuildMode buildMode)
{
    GoalPtr goal = derivationGoals[path].lock();
    if (!goal) {
        goal = std::make_shared<Goal>(path, wantedOutputs, *this, buildMode);
        derivationGoals[path] = goal;
        wakeUp(goal);
    } else
        goal.get()->addWantedOutputs(wantedOutputs);
    return goal;
}


std::shared_ptr<Goal> Worker::makeBasicDerivationGoal(const Path & drvPath,
    const BasicDerivation & drv, BuildMode buildMode)
{
    auto goal = std::make_shared<Goal>(drvPath, drv, *this, buildMode);
    wakeUp(goal);
    return goal;
}


static void removeGoal(GoalPtr goal, WeakGoalMap & goalMap)
{
    /* !!! inefficient */
    for (WeakGoalMap::iterator i = goalMap.begin();
         i != goalMap.end(); )
        if (i->second.lock() == goal) {
            WeakGoalMap::iterator j = i; ++j;
            goalMap.erase(i);
            i = j;
        }
        else ++i;
}


void Worker::removeGoal(GoalPtr goal)
{
    nix::removeGoal(goal, derivationGoals);
    if (topGoals.find(goal) != topGoals.end()) {
        topGoals.erase(goal);
        /* If a top-level goal failed, then kill all other goals
           (unless keepGoing was set). */
        if (goal->getExitCode() == Goal::ecFailed && !settings.keepGoing)
            topGoals.clear();
    }

    /* Wake up goals waiting for any goal to finish. */
    for (auto & i : waitingForAnyGoal) {
        GoalPtr goal = i.lock();
        if (goal) wakeUp(goal);
    }

    waitingForAnyGoal.clear();
}


void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    addToWeakGoals(awake, goal);
}


unsigned Worker::getNrLocalBuilds()
{
    return nrLocalBuilds;
}


void Worker::waitForBuildSlot(GoalPtr goal)
{
    debug("wait for build slot");
    if (getNrLocalBuilds() < settings.maxBuildJobs)
        wakeUp(goal); /* we can do it right away */
    else
        addToWeakGoals(wantingToBuild, goal);
}


void Worker::waitForAnyGoal(GoalPtr goal)
{
    debug("wait for any goal");
    addToWeakGoals(waitingForAnyGoal, goal);
}


void Worker::waitForAWhile(GoalPtr goal)
{
    debug("wait for a while");
    addToWeakGoals(waitingForAWhile, goal);
}


void Worker::run(const Goals & _topGoals)
{
    for (auto & i : _topGoals) topGoals.insert(i);

    startNest(nest, lvlDebug, format("entered goal loop"));

    while (1) {

        /* Call every wake goal (in the ordering established by
           CompareGoalPtrs). */
        while (!awake.empty() && !topGoals.empty()) {
            Goals awake2;
            for (auto & i : awake) {
                GoalPtr goal = i.lock();
                if (goal) awake2.insert(goal);
            }
            awake.clear();
            for (auto & goal : awake2) {
                goal->work();
                if (topGoals.empty()) break; // stuff may have been cancelled
            }
        }

        if (topGoals.empty()) break;

        Genode::Signal signal = sigRec.wait_for_signal();
        for (auto & i : builderPending) {
            GoalPtr goal = i.lock();
            if (goal && signal.context() == goal->context()) wakeUp(goal);
        }

    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
    assert(!settings.keepGoing || wantingToBuild.empty());
}


unsigned int Worker::exitStatus()
{
    return timedOut ? 101 : (permanentFailure ? 100 : 1);
}


void Worker::realize(GoalPtr goal)
{
    // slash hack
    char const *name = goal->getDrvPath().c_str();
    while (*name == '/') ++name;

    store.store_session().realize(name, sigRec.manage(goal->context()));
    addToWeakGoals(builderPending, goal);
}


//////////////////////////////////////////////////////////////////////


void Store::buildPaths(const PathSet & drvPaths, BuildMode buildMode)
{
    startNest(nest, lvlDebug, format("building %1%") % showPaths(drvPaths));

    Worker worker(*this);

    Goals goals;
    for (auto & i : drvPaths) {
        DrvPathWithOutputs i2 = parseDrvPathWithOutputs(i);
        goals.insert(worker.makeDerivationGoal(i2.first, i2.second, buildMode));
    }

    worker.run(goals);

    PathSet failed;
    for (auto & i : goals)
        if (i->getExitCode() == Goal::ecFailed) {
            Goal * i2 = i.get();
            failed.insert(i2->getDrvPath());
        }

    if (!failed.empty())
        throw Error(format("build of %1% failed") % showPaths(failed), worker.exitStatus());
}


BuildResult Store::buildDerivation(const Path & drvPath, const BasicDerivation & drv,
    BuildMode buildMode)
{
    startNest(nest, lvlDebug, format("building %1%") % showPaths({drvPath}));

    Worker worker(*this);
    auto goal = worker.makeBasicDerivationGoal(drvPath, drv, buildMode);

    BuildResult result;

    try {
        worker.run(Goals{goal});
        result = goal->getResult();
    } catch (Error & e) {
        result.status = BuildResult::MiscFailure;
        result.errorMsg = e.msg();
    }

    return result;
}



void Store::ensurePath(const Path & path)
{
    /* If the path is already valid, we're done. */
    if (isValidPath(path)) return;

    Worker worker(*this);
    GoalPtr goal = worker.makeDerivationGoal(path, StringSet());
    Goals goals = singleton<Goals>(goal);

    worker.run(goals);

    if (goal->getExitCode() != Goal::ecSuccess)
        throw Error(format("path ‘%1%’ does not exist and cannot be created") % path, worker.exitStatus());
}


/****************************************
void Store::repairPath(const Path & path)
{
    Worker worker(*this);
    GoalPtr goal = worker.makeGoal(path, true);
    Goals goals = singleton<Goals>(goal);

    worker.run(goals);

    if (goal->getExitCode() != Goal::ecSuccess)
        throw Error(format("cannot repair path ‘%1%’") % path, worker.exitStatus());
}
****************************************/

}
