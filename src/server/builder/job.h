/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 *
 * ********************
 * ** Ram allocation **
 * ********************
 *
 * TODO:
 * Each job's quota is [R / (2^N)] where
 * R is total ram available and N is the
 * position of the job in the queue.
 *
 * TODO: put a timeout on jobs
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
		Derivation             _drv;
		Lock                   _lock;
		List<Listener>         _listeners;
		Signal_rpc_member<Job> _exit_dispatcher;
		Child                 *_child;

	public:

		/**
		 * Constructor
		 * \param ep   entrypoint for child exit signal dispatcher
		 * \param drv  derivation that this job realizes
		 */
		Job(Server::Entrypoint &ep,

		    /* Arguments for Derivation. */
		    Genode::Allocator    *alloc,
		    File_system::Session &store_fs,
		    char           const *name)
		:
			_drv(alloc, store_fs, name),
			_exit_dispatcher(ep, *this, &Job::end),
			_child(0)
		{
			strncpy(_name, name, sizeof(_name));
		};

		/**
		 * Handler for the child exit.
		 */
		void end(unsigned)
		{
			Lock::Guard guard(_lock);

			if (_child) {
				destroy(Genode::env()->heap(), _child);
				_child = 0;
			}
			/*
			 * Notify the listeners then remove them.
			 */
			for (Listener *curr = _listeners.first(); curr; curr = curr->next()) {
				curr->notify();
				destroy(Genode::env()->heap(), curr);
			}
		}

		~Job() { end(0); }

		char const *name() { return _name; };

		void add_listener(Genode::Signal_context_capability sigh)
		{
			Genode::Lock::Guard guard(_lock);

			Listener *listener = new (Genode::env()->heap())
				Listener(sigh);
			_listeners.insert(listener);
		}

		bool waiting()
		{
			Genode::Lock::Guard guard(_lock);

			if (_child)
				return false;

			for (Listener *l = _listeners.first(); l; l = l->next()) {
				if (l->valid())
					return true;
				else
					_listeners.remove(l);
					destroy(Genode::env()->heap(), l);
			}
			return false;
		}

		bool wanted()
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

		void start(File_system::Session &fs,
		           Cap_session          &cap,
		           Ram                  &ram)
		{
			if (_child) return;

			_child = new (Genode::env()->heap())
				Child(_name, fs, cap, _drv, ram, _exit_dispatcher);
		}
};


// TODO: jobs need to be removed when they are done, do it on yield
class Builder::Job_queue : private Genode::List<Job>
{
	private:

		Cap_connection        _cap;
		Signal_receiver       _sig_rec;
		Signal_context        _yield_broadcast_sig_ctx;
		Signal_context        _resource_avail_sig_ctx;
		//Signal_context        _yield_response_sig_ctx;
		Ram                   _ram;
		Lock                  _lock;
		Server::Entrypoint   &_ep;
		File_system::Session &_fs;

		//Signal_context_capability _yield_response_sig_cap;

		Job *lookup_name(char const *name)
		{
			for (Job *curr = first(); curr; curr = curr->next())
				if (strcmp(name, curr->name(), MAX_NAME_LEN) == 0)
					return curr;
			return 0;
		}

	public:

		Job_queue(Server::Entrypoint   &ep,
		          File_system::Session &fs)
		:
			_ram(0, // TODO: add and subtract session donation to preservation?
			     _sig_rec.manage(&_yield_broadcast_sig_ctx),
			     _sig_rec.manage(&_resource_avail_sig_ctx)),
			//_yield_response_sig_cap(_sig_rec.manage(&_yield_response_sig_ctx)),
			_ep(ep),
			_fs(fs)
		{ }

		void queue(char const                       *drv_name,
		           Genode::Signal_context_capability sigh)
		{
			Lock::Guard guard(_lock);

			Job *job = lookup_name(drv_name);
			if (!job) try {
				job = new (env()->heap())
					Job(_ep, env()->heap(), _fs, drv_name);
				insert(job);
			} catch (...) {
				PERR("error queueing %s", drv_name);
				throw;
			}

			job->add_listener(sigh);
		}

		/**
		 * Start new jobs
		 */
		void process()
		{
			Lock::Guard guard(_lock);

			try {
				for (Job *job = first(); job; job = job->next()) {
					if (job->waiting())
						job->start(_fs, _cap, _ram); //, yield_response_sig_cap);
					else if (!job->wanted())
						job->end(0);
				}
			} catch (Ram::Transfer_quota_failed) {
				Ram::Status status = _ram.status();
				PERR("%lu/%lu bytes of quota used, cannot start more jobs",
				     status.used, status.quota);
			}
		}
};

#endif
