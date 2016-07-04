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

#ifndef _NIX_STORE__BUILD_JOB_H_
#define _NIX_STORE__BUILD_JOB_H_

/* Genode includes */
#include <os/signal_rpc_dispatcher.h>
#include <base/lock.h>
#include <util/fifo.h>
#include <util/string.h>

/* Nix includes */
#include <nix/types.h>
#include <nix_store/derivation.h>

/* Local includes */
#include "build_child.h"

namespace Nix_store {

	using namespace Genode;

	class Job;
	class Jobs;

};


/**
 * A job wraps a child and informs its
 * listeners of the childs completion.
 */
class Nix_store::Job : public Fifo<Job>::Element
{
	/* Job is thread-safe if methods are only exported to Jobs */
	friend Jobs;

	private:

		Nix_store::Name const     _name;
		Signal_context_capability _sigh;


	public:

		/**
		 * Constructor
		 */
		Job(char const *name, Genode::Signal_context_capability sigh)
		: _name(name), _sigh(sigh) { }

		/**
		 * Destructor
		 */
		~Job()
		{
			if (_sigh.valid())
				Genode::Signal_transmitter(_sigh).submit();
		}

		char const *name() { return _name.string(); }

		bool abandoned() const { return !_sigh.valid(); }
};


class Nix_store::Jobs : private Genode::Fifo<Job>
{
	private:

		Genode::Env             &_env;
		Genode::Allocator       &_alloc;

		Genode::Rom_connection _ldso_rom { "ld.lib.so" };
		Genode::Rom_dataspace_capability _ldso_ds = _ldso_rom.dataspace();

		Lock                     _lock;
		File_system::Session    &_fs;

		Genode::Lazy_volatile_object<Nix_store::Child> _child;

		/**
		 * Handle resource announcement from parent
		 */
		void _handle_resource()
		{
			{
				Lock::Guard guard(_lock);
				if (_child.constructed()) {
					_child->upgrade_ram();
					return;
				}
			}

			process();
		}

		Genode::Signal_handler<Jobs> _resource_handler
			{ _env.ep(), *this, &Jobs::_handle_resource };

		/**
		 * Handle yield signal from parent
		 */
		void _handle_yield()
		{
			Genode::Parent::Resource_args args = _env.parent().yield_request();

			size_t quota_request =
				Arg_string::find_arg(args.string(), "ram_quota").ulong_value(0);

			/*
			 * If we are low on memory, kill the job and schedule a restart,
			 * otherwise let the parent withdrawl what is available.
			 *
			 * Note that a yield signal is not sent to the child because that
			 * would violate the purity of the child environment.
			 */
			if ((_env.ram().avail() < QUOTA_STEP) &&
			    (quota_request > QUOTA_STEP))
			{
				Lock::Guard guard(_lock);

				if (_child.constructed())
					_child.destruct();

				/* get the job on top of the queue, but don't remove it */
				if (Job *job = head())
					Genode::log(job->name(), " killed to yield resources");
			}

			_env.parent().yield_response();
		}

		Genode::Signal_handler<Jobs> _yield_handler
			{ _env.ep(), *this, &Jobs::_handle_yield };

		/**
		 * Handle exit signal from the child
		 */
		void _handle_exit()
		{
			{
				Lock::Guard guard(_lock);
				_child.destruct();

				/* Job destructor notifies listeners */
				if (Job *job = dequeue())
					destroy(_alloc, job);
			}

			process();
		}

		Genode::Signal_handler<Jobs> _exit_handler
			{ _env.ep(), *this, &Jobs::_handle_exit };

	public:

		Jobs(Genode::Env &env, Genode::Allocator &alloc, File_system::Session &fs)
		:
			_env(env), _alloc(alloc), _fs(fs)
		{
			env.parent().resource_avail_sigh(_resource_handler);
			env.parent().yield_sigh(_yield_handler);
		}

		/**
		 * Process the queue
		 */
		void process()
		{
			Lock::Guard guard(_lock);

			if (_child.constructed() || empty()) return;

			Job *job = head();
			while (job->abandoned()) {
				dequeue();
				destroy(_alloc, job);
				job = head();
				if (!job) return;
			}

			/*
			 * If RAM quota is sufficient then start a job,
			 * otherwise make a non-blocking upgrade request.
			 */
			if (_env.ram().avail() > QUOTA_STEP+QUOTA_RESERVE) {
				_child.construct(job->name(), _env, _fs, _exit_handler, _ldso_ds);
				return;
			}

			Genode::log("requesting more RAM before starting job...");

			char arg_buf[32];
			Genode::snprintf(arg_buf, sizeof(arg_buf),
			                 "ram_quota=%ld", QUOTA_STEP);
			_env.parent().resource_request(arg_buf);
		}

		void queue(char const                       *drv_name,
		           Genode::Signal_context_capability sigh)
		{
			{
				Lock::Guard guard(_lock);

				try {
					Job *job = new (_alloc) Job(drv_name, sigh);
					enqueue(job);
				} catch (Aterm::Parser::Malformed_element) {
					Genode::error("canceling job with malformed derivation at ", drv_name);
					throw Invalid_derivation();
				} catch (...) {
					Genode::error("error queueing %s", drv_name);
					throw;
				}
			}

			process();
		}
};

#endif
