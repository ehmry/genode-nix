/*
 * \brief  Job queuing
 * \author Emery Hemingway
 * \date   2015-03-13
 *
 * Jobs are queued in simplex FIFO to keep the implementation
 * simple. By not defining an internal resource sharing policy,
 * the user is free to set affinity, memory, and priority
 * policy externally in the parent or in a job multiplexer.
 */

#ifndef _BUILDER__JOB_H_
#define _BUILDER__JOB_H_

/* Genode includes */
#include <os/signal_rpc_dispatcher.h>
#include <base/lock.h>
#include <util/fifo.h>
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
	class Jobs;

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

		~Listener()
		{
			if (_sigh.valid())
				Signal_transmitter(_sigh).submit();
		};

		bool valid() const { return _sigh.valid(); }
};


/**
 * A job wraps a child and informs its
 * listeners of the childs completion.
 */
class Builder::Job : public Fifo<Job>::Element
{
	/* Job is thread-safe if methods are only exported to Jobs */
	friend Jobs;

	private:

		typedef Genode::String<MAX_NAME_LEN> Name;

		Name      const _name;
		Derivation      _drv;
		List<Listener>  _listeners;
		Child          *_child;

	protected:

		bool match(char const *name) { return _name == name; }

		void add_listener(Genode::Signal_context_capability sigh)
		{
			Listener *listener = new (Genode::env()->heap())
				Listener(sigh);
			_listeners.insert(listener);
		}

		bool abandoned()
		{
			for (Listener *l = _listeners.first(); l; l = l->next()) {
				if (l->valid())
					return false;
				else
					destroy(Genode::env()->heap(), l);
			}
			return true;
		}

		void start(File_system::Session              &fs,
		           Genode::Signal_context_capability  exit_sigh)
		{
			try {
				_child = new (Genode::env()->heap())
					Child(_name.string(), fs, _drv, exit_sigh);
			} catch (Missing_dependency) {
				PERR("missing dependency for %s", _name.string());
				Signal_transmitter(exit_sigh).submit();
			} catch (...) {
				PERR("failed to start job for %s", _name.string());
			}
		}

		void kill()
		{
			if (_child) {
				destroy(Genode::env()->heap(), _child);
				_child = nullptr;
				return;
			}
		}

	public:

		/**
		 * Constructor
		 */
		Job(char const *name) : _name(name), _drv(name), _child(nullptr) { }

		/**
		 * Destructor
		 */
		~Job()
		{
			if (_child)
				destroy(Genode::env()->heap(), _child);

			/* Listener destructor notifies */
			for (Listener *l = _listeners.first(); l; l = l->next())
				destroy(Genode::env()->heap(), l);
		}

		char const *name() { return _name.string(); }

		/**
		 * Upgrade and notify the child
		 */
		void upgrade_ram() { _child->upgrade_ram(); }
};


class Builder::Jobs : private Genode::Fifo<Job>
{
	private:

		Signal_rpc_member<Jobs>  _resource_dispatcher;
		Signal_rpc_member<Jobs>  _yield_dispatcher;
		Signal_rpc_member<Jobs>  _exit_dispatcher;
		Lock                     _lock;
		File_system::Session    &_fs;
		bool                     _pending;

		/**
		 * Handle resource announcement from parent
		 */
		void _resource_handler(unsigned)
		{
			{
				Lock::Guard guard(_lock);
				if (_pending) {
					head()->upgrade_ram();
					return;
				}
			}

			process();
		}

		/**
		 * Handle yield signal from parent
		 */
		void _yield_handler(unsigned)
		{
			Genode::Parent::Resource_args args =
				Genode::env()->parent()->yield_request();

			size_t quota_request =
				Arg_string::find_arg(args.string(), "ram_quota").ulong_value(0);

			/*
			 * If we are low on memory, kill the job and schedule a restart,
			 * otherwise let the parent withdrawl what is available.
			 *
			 * Note that a yield signal is not sent to the child because that
			 * would violate the purity of the child environment.
			 */
			if ((Genode::env()->ram_session()->avail() < QUOTA_STEP) &&
			    (quota_request > QUOTA_STEP))
			{
				Lock::Guard guard(_lock);

				/* get the job on top of the queue, but don't remove it */
				Job *job = head();
				if (job) {
					job->kill();
					PERR("%s killed to yield resources", job->name());
					_pending = false;
				}
			}

			Genode::env()->parent()->yield_response();
		}

		/**
		 * Handle exit signal from the child
		 */
		void _exit_handler(unsigned)
		{
			{
				Lock::Guard guard(_lock);

				/* Job destructor notifies listeners */
				destroy(env()->heap(), dequeue());

				_pending = false;
			}

			process();
		}

	public:

		Jobs(Server::Entrypoint &ep, File_system::Session &fs)
		:
			_resource_dispatcher(ep, *this, &Jobs::_resource_handler),
			_yield_dispatcher(   ep, *this, &Jobs::_yield_handler),
			_exit_dispatcher(    ep, *this, &Jobs::_exit_handler),
			_fs(fs),
			_pending(false)
		{
			Genode::env()->parent()->resource_avail_sigh(_resource_dispatcher);
			Genode::env()->parent()->yield_sigh(_yield_dispatcher);
		}

		/**
		 * Process the queue
		 */
		void process()
		{
			Lock::Guard guard(_lock);

			if (_pending || empty()) return;

			Job *job = head();
			while (job->abandoned()) {
				dequeue();
				destroy(Genode::env()->heap(), job);
				job = head();
				if (!job) return;
			}

			/*
			 * If RAM quota is sufficient then start a job,
			 * otherwise make a non-blocking upgrade request.
			 */
			if (Genode::env()->ram_session()->avail() >
			    QUOTA_STEP+QUOTA_RESERVE)
			{
				job->start(_fs, _exit_dispatcher);
				_pending = true;
				return;
			}

			PLOG("requesting more RAM before starting job...");

			char arg_buf[32];
			Genode::snprintf(arg_buf, sizeof(arg_buf),
			                 "ram_quota=%ld", QUOTA_STEP);
			Genode::env()->parent()->resource_request(arg_buf);
		}

		void queue(char const                       *drv_name,
		           Genode::Signal_context_capability sigh)
		{
			{
				Lock::Guard guard(_lock);

				Job *job = nullptr;
				for (Job *j = head(); job; job = job->next())
					if (j->match(drv_name)) {
						job = j;
						break;
					}

				if (!job) try {
					job = new (env()->heap()) Job(drv_name);
					enqueue(job);
				} catch (Aterm::Parser::Malformed_element) {
					PERR("canceling job with malformed derivation file at %s", drv_name);
					throw Invalid_derivation();
				} catch (...) {
					PERR("error queueing %s", drv_name);
					throw;
				}

				job->add_listener(sigh);
			}

			process();
		}
};

#endif
