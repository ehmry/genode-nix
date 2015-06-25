/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 *
 * ********************
 * ** Ram allocation **
 * ********************
 *
 * Each job's quota is [R / (2^N)] where
 * R is total ram available and N is the
 * position of the job in the queue.
 *
 * TODO: reduce the hash to a shorter length,
 * if this means jobs don't get queued, no big deal,
 * thats a temporary failure.
 */

#ifndef _BUILDER__JOB_H_
#define _BUILDER__JOB_H_

/* Genode includes */
#include <cli_monitor/ram.h>
#include <cap_session/connection.h>
#include <os/signal_rpc_dispatcher.h>
#include <base/lock.h>
#include <util/list.h>
#include <util/string.h>


/* Nix includes */
#include <nix/types.h>

/* Local includes */
#include "derivation.h"
#include "child.h"

namespace Builder {

	using namespace Genode;

	class Listener;
	class Job;
	class Job_factory;
	class Job_queue;

};

/**
 * The Listener is used to notify a client or another
 * job when this job completes. Its validity of the
 * signal context capability it wraps is also used
 * as a form of reference count.
 *
 * From <file_system/listener.h>.
 */
class Builder::Listener : public Genode::List<Listener>::Element
{
	private:

		Signal_context_capability _sigh;

	public:

		Listener(Genode::Signal_context_capability sigh)
		: _sigh(sigh) { }

		void notify()
		{
			if (_sigh.valid())
				Signal_transmitter(_sigh).submit();
		}

		bool valid() const { return _sigh.valid(); }
};

/**
 * A job wraps a child and informs its listeners
 * of the childs completion.
 *
 * This or some other class ought be abstracted
 * for Noux jobs, native jobs, and fetch jobs.
 */
class Builder::Job : public List<Job>::Element
{
	private:

		char                   _name[MAX_NAME_LEN];
		File_system::Session  &_fs;
		Derivation             _drv;
		Lock                   _lock;
		List<Listener>         _listeners;
		Signal_rpc_member<Job> _exit_dispatcher;
		Cap_session           &_cap;
		Child                 *_child;

		/**
		 * Handler for the child exit.
		 */
		void _exit(unsigned)
		{
			_lock.lock();

			destroy(Genode::env()->heap(), _child);
			_child = 0;

			/* TODO: how does the job get removed? */

			/*
			 * Notify the listeners then remove them.
			 */
			for (Listener *curr = _listeners.first(); curr; curr = curr->next()) {
				curr->notify();
				destroy(Genode::env()->heap(), curr);
			}
			_lock.unlock();

			// TODO: propagate to dependers
		}

		void kill()
		{
			if (!_child) return;
			destroy(Genode::env()->heap(), _child);
			_child = 0;
		}


	public:

		/**
		 * Constructor
		 * \param ep   entrypoint for child exit signal dispatcher
		 * \param drv  derivation that this job realizes
		 * \param ctx  client or dependency signal handler
		 */
		Job(Server::Entrypoint &ep,
		    Cap_session        &cap,

		    /* Arguments for Derivation. */
		    Genode::Allocator        *alloc,
		    File_system::Session     &store_fs,

		    /* Arguments from a client session. */
		    Name                       const &name,
		    Genode::Signal_context_capability ctx)
		:
			_fs(store_fs),
			_drv(alloc, _fs, name.string()),
			_exit_dispatcher(ep, *this, &Job::_exit),
			_cap(cap),
			_child(0)
		{
			strncpy(_name, name.string(), sizeof(_name));
			/* Register the first signal context so the job isn't dropped. */
			sigh(ctx);
		};

		~Job()
		{
			_lock.lock();

			kill();

			int dbg_destroy = 0;
			int dbg_notify = 0;

			for (Listener *l = _listeners.first(); l; l = l->next()) {
				/*
				 * Will the builder destruct signal exit,
				 * making these listeners invalid anyway?
				 */
				if (l->valid()) {
					++dbg_notify;
					l->notify();
				}

				destroy(Genode::env()->heap(), l);
				++dbg_destroy;
			}
			PDBG("notified %d listerners, destroyed %d", dbg_notify, dbg_destroy);
		}

		char const *name() { return _name; };

		void sigh(Genode::Signal_context_capability ctx)
		{
			Genode::Lock::Guard guard(_lock);

			Listener *listener = new (Genode::env()->heap())
				Listener(ctx);
			_listeners.insert(listener);
		}

		bool active()
		{
			Genode::Lock::Guard guard(_lock);

			for (Listener *l = _listeners.first(); l; l = l->next()) {
				if (l->valid())
					return true;
				else
					_listeners.remove(l);
					destroy(Genode::env()->heap(), l);
			}
			return false;
		}

		void wake()
		{
			//for (Input *input = drv.Inputs(); input; input = input->next())
			/* Run through the inputs and queue anything that is missing. */
		}

		void start(Ram &ram) //, Signal_context_capability yield_sigh)
		{
			if (_child) return;

			_child = new (Genode::env()->heap())
				Child(name(), _fs, _cap, _drv, ram, _exit_dispatcher);
		}
};


// TODO: jobs need to be removed when they are done.
class Builder::Job_queue : private Genode::List<Job>
{

	private:

		// TODO: make static members?
		Cap_connection      _cap;
		Signal_receiver     _sig_rec;
		Signal_context      _yield_broadcast_sig_ctx;
		Signal_context      _resource_avail_sig_ctx;
		Signal_context      _yield_response_sig_ctx;
		Ram                 _ram;
		Lock                _lock;
		Server::Entrypoint &_ep;

		//Signal_context_capability _yield_response_sig_cap;

	public:

		Job_queue(Server::Entrypoint &ep)
		: 
			_ram(0, // TODO: add and subtract session donation to preservation?
			     _sig_rec.manage(&_yield_broadcast_sig_ctx),
			     _sig_rec.manage(&_resource_avail_sig_ctx)),
			//_yield_response_sig_cap(_sig_rec.manage(&_yield_response_sig_ctx)),
			_ep(ep)
		{ }

		Job* create(Genode::Allocator                *alloc,
		            File_system::Session             &fs,
		            Name const                       &drv_name,
		            Genode::Signal_context_capability sigh)
		{
			Job *job = new (Genode::env()->heap())
				Job(_ep, _cap, alloc, fs, drv_name, sigh);

			insert(job);
			return job;
		}

	/**
	 * Process the queue; start new jobs and drop stale jobs.
	 */
	void process()
	{
		/*
		 * Ensure jobs are started in serial.
		 */
		Lock::Guard guard(_lock);
		PDBG("not really implemented, just starting them all");
		try {
			for (Job *curr = first(); curr; curr = curr->next())
				curr->start(_ram); //, yield_response_sig_cap);
		} catch (Ram::Transfer_quota_failed) {
			Ram::Status status = _ram.status();
			PERR("%lu/%lu bytes of quota used, cannot start more jobs",
			     status.used, status.quota);
			// TODO: how many jobs are running?
		}
	}

	Job *lookup_name(char const *name)
	{
		for (Job *curr = first(); curr; curr = curr->next())
			if (strcmp(name, curr->name(), MAX_NAME_LEN) == 0)
				return curr;
		return 0;
	}
};

#endif
