#include "config.h"

#include "references.hh"
#include "pathlocks.hh"
#include "misc.hh"
#include "globals.hh"
#include "util.hh"
#include "archive.hh"
#include "affinity.hh"

#include "store.hh"

#include <map>
#include <sstream>
#include <algorithm>

#include <cstring>


#include <builder_session/builder_session.h>


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
typedef map<string, WeakGoalPtr> WeakGoalMap;


class Goal : public std::enable_shared_from_this<Goal>
{
private:
    /* The name of the derivation. */
    string drvName;

    /* The specific outputs that we need to build.  Empty means all of
       them. */
    StringSet wantedOutputs;

    /* Whether additional wanted outputs have been added. */
    bool needRestart;

    /* The derivation stored at drvName. */
    Derivation drv;

    /* The remainder is state held during the build. */

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

    /* All inputs that are regular files. */
    PathSet regularInputPaths;

    /* Whether this is a fixed-output derivation. */
    bool fixedOutput;

    typedef void (DerivationGoal::*GoalState)();
    GoalState state;

    /* If we're repairing without a chroot, there may be outputs that
       are valid but corrupt.  So we redirect these outputs to
       temporary paths. */
    PathSet redirectedBadOutputs;

public:
    typedef enum {ecBusy, ecSuccess, ecFailed} ExitCode;

protected:

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

    Genode::Signal_context sig_ctx;
    Genode::Signal_context_capability sig_cap;

    Goal(Worker & worker)
    : worker(worker)
    , nrFailed(0)
    , exitCode(ecBusy)
    { }

    ~Goal()
    {
        trace("destroying goal");
        if (sig_cap.valid())
            worker.sig_rec.dissolve(&sig_ctx);
        trace("goal destroyed");
    }

public:
    virtual void work() = 0;

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

    /* Cancel the goal.  It should wake up its waiters, get rid of any
       running child processes that are being monitored by the worker
       (important!), etc. */
    virtual void cancel() = 0;

    virtual string key() = 0;

    Genode::Signal_context* signal_context() { return &sig_ctx; }

    Genode::Signal_context_capability manage(Genode::Signal_receiver &sig_rec);


protected:
    void amDone(ExitCode result);

public:

    void cancel();

    string key()
    {
        /* Ensure that derivations get built in order of their name,
           i.e. a derivation named "aardvark" always comes before
           "baboon". And substitution goals always happen before
           derivation goals (due to "b$"). */
        return "b$" + drvName.substr(34) + "$" + drvName;
    }

    void work();

    string getDrvName()
    {
        return drvName;
    }

    /* Add wanted outputs to an already existing derivation goal. */
    void addWantedOutputs(const StringSet & outputs);

private:
    /* The states. */
    void haveDerivation();
    void inputsRealised();
    void tryToBuild();
    void buildDone();

    /* Return the set of (in)valid paths. */
    PathSet checkPathValidity(bool returnValid);

    void repairClosure();

};


bool CompareGoalPtrs::operator() (const GoalPtr & a, const GoalPtr & b) {
    string s1 = a->key();
    string s2 = b->key();
    return s1 < s2;
}


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
    WeakGoals building;

    /* Number of build slots occupied.  This includes local builds and
       substitutions but not remote builds via the build hook. */
    unsigned int nrLocalBuilds;

    /* Maps used to prevent multiple instantiations of a goal for the
       same derivation / path. */
    WeakGoalMap goals;

    /* Goals waiting for busy paths to be unlocked. */
    WeakGoals waitingForAnyGoal;

public:

    /* Set if at least one derivation had a BuildError (i.e. permanent
       failure). */
    bool permanentFailure;

    /* Set if at least one derivation had a timeout. */
    bool timedOut;

    Store & store;

    Builder::Session & builder;

    Genode::Signal_receiver sig_rec;

    Worker(Store & store, Builder::Session & builder);
    ~Worker();

    /* Make a goal (with caching). */
    GoalPtr makeGoal(const string & drvName, const StringSet & wantedOutputs);

    /* Remove a dead goal. */
    void removeGoal(GoalPtr goal);

    /* Wake up a goal (i.e., there is something for it to do). */
    void wakeUp(GoalPtr goal);
    
    /* Start a goal building. */
    void startBuilding(GoalPtr goal);

    /* Return the number of local build and substitution processes
       currently running (but not remote builds via the build
       hook). */
    unsigned int getNrLocalBuilds();

    /* Loop until the specified top-level goals have finished. */
    void run(const Goals & topGoals);

    unsigned int exitStatus();
};


//////////////////////////////////////////////////////////////////////


void addToWeakGoals(WeakGoals & goals, GoalPtr p)
{
    // FIXME: necessary?
    // FIXME: O(n)
    foreach (WeakGoals::iterator, i, goals)
        if (i->lock() == p) return;
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
    waitees.clear();

    trace(format("waitee ‘%1%’ done; %2% left") %
        waitee->name % waitees.size());

    if (result == ecFailed) ++nrFailed;

    if (waitees.empty() || (result == ecFailed && !settings.keepGoing)) {

        /* If we failed and keepGoing is not set, remove all
           remaining waitees. */
        foreach (Goals::iterator, i, waitees) {
            GoalPtr goal = *i;
            WeakGoals waiters2;
            foreach (WeakGoals::iterator, j, goal->waiters)
                if (j->lock() != shared_from_this()) waiters2.push_back(*j);
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
    assert(result == ecSuccess || result == ecFailed);
    exitCode = result;
    foreach (WeakGoals::iterator, i, waiters) {
        GoalPtr goal = i->lock();
        if (goal) goal->waiteeDone(shared_from_this(), result);
    }
    waiters.clear();
    worker.removeGoal(shared_from_this());
}


void Goal::trace(const format & f)
{
    debug(format("%1%: %2%") % name % f);
}


Genode::Signal_context_capability Goal::manage(Genode::Signal_receiver &sig_rec)
{
    return sig_rec.manage(&sig_ctx);
}


//////////////////////////////////////////////////////////////////////


class DerivationGoal : public Goal
{};


DerivationGoal::DerivationGoal(const string & drvName, const StringSet & wantedOutputs, Worker & worker)
    : Goal(worker)
    , wantedOutputs(wantedOutputs)
    , needRestart(false)
{
    this->drvName = drvName;
    state = &DerivationGoal::haveDerivation;
    name = drvName;
    trace("created");
}

DerivationGoal::~DerivationGoal()
{
    trace("destroying DerivationGoal");
    if (sig_cap.valid())
        worker.sig_rec.dissolve(&sig_ctx);
    trace("DerivationGoal destroyed");
}


void DerivationGoal::cancel()
{
    if (settings.printBuildTrace)
        printMsg(lvlError, format("@ build cancelled %1%") % drvName);
    if (sig_cap.valid())
        worker.sig_rec.dissolve(&sig_ctx);
    amDone(ecFailed);
}


void DerivationGoal::work()
{
    (this->*state)();
}


void DerivationGoal::addWantedOutputs(const StringSet & outputs)
{
    /* If we already want all outputs, there is nothing to do. */
    if (wantedOutputs.empty()) return;

    if (outputs.empty()) {
        wantedOutputs.clear();
        needRestart = true;
    } else
        foreach (StringSet::const_iterator, i, outputs)
            if (wantedOutputs.find(*i) == wantedOutputs.end()) {
                wantedOutputs.insert(*i);
                needRestart = true;
            }
}


void DerivationGoal::haveDerivation()
{
    trace("loading derivation");

    if (nrFailed != 0) {
        printMsg(lvlError, format("cannot build missing derivation ‘%1%’") % drvName);
        amDone(ecFailed);
        return;
    }

    /* Ask the store if it has the derivation */
    assert(worker.store.isValidPath(drvName));

    /* Get the derivation. */
    drv = derivationFromPath(worker.store, drvName);

    /* Check what outputs paths are not already valid. */
    PathSet invalidOutputs = checkPathValidity(false);

    /* If they are all valid, then we're done. */
    if (invalidOutputs.size() == 0) {
        amDone(ecSuccess);
        return;
    }

    /* The inputs must be built before we can build this goal. */
    foreach (DerivationInputs::iterator, i, drv.inputDrvs)
        addWaitee(worker.makeDerivationGoal(i->first, i->second));

    if (waitees.empty()) /* to prevent hang (no wake-up event) */
        inputsRealised();
    else
        state = &DerivationGoal::inputsRealised;
}

void DerivationGoal::inputsRealised()
{
    trace("all inputs realised");

    if (nrFailed != 0) {
        printMsg(lvlError,
            format("cannot build derivation ‘%1%’: %2% dependencies couldn't be built")
            % drvName % nrFailed);
        amDone(ecFailed);
        return;
    }

    /* Gather information necessary for computing the closure and/or
       running the build hook. */

    /* The outputs are referenceable paths. */
    foreach (DerivationOutputs::iterator, i, drv.outputs) {
        debug(format("building path ‘%1%’") % i->second.path);
        allPaths.insert(i->second.path);
    }

    /* Determine the full set of input paths. */

    /* First, the input derivations. */
    foreach (DerivationInputs::iterator, i, drv.inputDrvs) {
        /* Add the relevant output closures of the input derivation
           `*i' as input paths.  Only add the closures of output paths
           that are specified as inputs. */
        assert(worker.store.isValidPath(i->first));
        Derivation inDrv = derivationFromPath(worker.store, i->first);
        foreach (StringSet::iterator, j, i->second)
            if (inDrv.outputs.find(*j) != inDrv.outputs.end())
                computeFSClosure(worker.store, inDrv.outputs[*j].path, inputPaths);
            else
                throw Error(
                    format("derivation ‘%1%’ requires non-existent output ‘%2%’ from input derivation ‘%3%’")
                    % drvName % *j % i->first);
    }

    /* Second, the input sources. */
    foreach (PathSet::iterator, i, drv.inputSrcs)
        computeFSClosure(worker.store, *i, inputPaths);

    debug(format("added input paths %1%") % showPaths(inputPaths));

    allPaths.insert(inputPaths.begin(), inputPaths.end());

    /* Is this a fixed-output derivation? */
    fixedOutput = true;
    foreach (DerivationOutputs::iterator, i, drv.outputs)
        if (i->second.hash == "") fixedOutput = false;

    /* Okay, try to build.  Note that here we don't wait for a build
       slot to become available, since we don't need one if there is a
       build hook. */
    state = &DerivationGoal::tryToBuild;
    worker.wakeUp(shared_from_this());
}


static bool canBuildLocally(const string & platform)
{
    return platform == settings.thisSystem
#if __linux__
        || (platform == "i686-linux" && settings.thisSystem == "x86_64-linux")
#endif
        ;
}


static string get(const StringPairs & map, const string & key, const string & def = "")
{
    StringPairs::const_iterator i = map.find(key);
    return i == map.end() ? def : i->second;
}


bool willBuildLocally(const Derivation & drv)
{
    return drv.platform == SYSTEM;
}


void DerivationGoal::tryToBuild()
{
    trace("trying to build");

    try {
        worker.startBuilding(shared_from_this());
    } catch (Builder::Invalid_derivation) {
        printMsg(lvlError, format("builder reports derivation %1% was invalid") % drvName);
        if (settings.printBuildTrace)
            printMsg(lvlError, format("@ build-failed %1% - %2% %3%")
                % drvName % 0 % "invalid derivation");
        worker.permanentFailure = true;
        amDone(ecFailed);
        return;
    }

    /* This state will be reached when we get a signal back from the builder. */
    state = &DerivationGoal::buildDone;
}


void DerivationGoal::buildDone()
{
    trace("build done");

    debug(format("builder process for ‘%1%’ finished") % drvName);

    if (settings.printBuildTrace)
        printMsg(lvlError, format("@ build-succeeded %1% -") % drvName);

    amDone(ecSuccess);
}


PathSet DerivationGoal::checkPathValidity(bool returnValid)
{
    PathSet result;
    foreach (DerivationOutputs::iterator, i, drv.outputs) {
        if (!wantOutput(i->first, wantedOutputs)) continue;
        bool good =
            worker.store.isValidPath(i->second.path);
        if (good == returnValid) result.insert(i->second.path);
    }
    return result;
}


//////////////////////////////////////////////////////////////////////


static bool working = false;


Worker::Worker(Store & store, Builder::Session & builder)
    : store(store)
    , builder(builder)
{
    /* Debugging: prevent recursive workers. */
    if (working) abort();
    working = true;
    nrLocalBuilds = 0;
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


GoalPtr Worker::makeDerivationGoal(const string & name, const StringSet & wantedOutputs)
{
    string drvName = name;
    while (drvName.front() == '/') drvName.erase(0, 1);

    GoalPtr goal = derivationGoals[drvName].lock();
    if (!goal) {
        goal = GoalPtr(new DerivationGoal(drvName, wantedOutputs, *this));
        derivationGoals[drvName] = goal;
        wakeUp(goal);
    } else
        (dynamic_cast<DerivationGoal *>(goal.get()))->addWantedOutputs(wantedOutputs);
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
    foreach (WeakGoals::iterator, i, waitingForAnyGoal) {
        GoalPtr goal = i->lock();
        if (goal) wakeUp(goal);
    }

    waitingForAnyGoal.clear();
}


void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    addToWeakGoals(awake, goal);
}

void Worker::startBuilding(GoalPtr goal)
{
    goal->trace("woken up");
    builder.realize(goal->getName().c_str(), goal->manage(sig_rec));
    addToWeakGoals(building, goal);
}

unsigned Worker::getNrLocalBuilds()
{
    return nrLocalBuilds;
}


void Worker::run(const Goals & _topGoals)
{
    foreach (Goals::iterator, i,  _topGoals) topGoals.insert(*i);

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

        /*
         * TODO:
         * Register a timeout on the receiver.
         */

	// TODO and if nothing is building?

        if (!building.empty()) {
            Genode::Signal signal = sig_rec.wait_for_signal();
            for (auto & i : building) {
                GoalPtr goal = i.lock();
                if (!goal) continue;
                if (goal->signal_context() == signal.context()) {
                    wakeUp(goal);
                    break;
                }
            }
        }
    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
}


unsigned int Worker::exitStatus()
{
    return timedOut ? 101 : (permanentFailure ? 100 : 1);
}


//////////////////////////////////////////////////////////////////////


void Store::buildPaths(const PathSet & drvPaths, BuildMode buildMode)
{
    startNest(nest, lvlDebug,
        format("building %1%") % showPaths(drvPaths));

    Worker worker(*this, _builder);

    Goals goals;
    foreach (PathSet::const_iterator, i, drvPaths) {
        DrvPathWithOutputs i2 = parseDrvPathWithOutputs(*i);
        if (isDerivation(i2.first)) {
            // TODO: strip the slash here, or earlier?
            string drvName = i2.first;
            while (drvName.front() == '/') drvName.erase(0, 1);
            goals.insert(worker.makeDerivationGoal(drvName, i2.second));
        }
    }

    worker.run(goals);

    PathSet failed;
    foreach (Goals::iterator, i, goals)
        if ((*i)->getExitCode() == Goal::ecFailed) {
            DerivationGoal * i2 = dynamic_cast<DerivationGoal *>(i->get());
            if (i2) failed.insert(i2->getDrvName());
        }

    if (!failed.empty())
        throw Error(format("build of %1% failed") % showPaths(failed), worker.exitStatus());
}


}
