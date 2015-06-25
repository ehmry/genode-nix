/*
 * \brief  Builder job managment
 * \author Emery Hemingway
 * \date   2015-06-13
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _JOB_H_
#define _JOB_H_

/* Genode includes */
#include <builder_session/builder_session.h>
#include <base/signal.h>
#include <util/list.h>


class Jobs
{
	private:

		struct Job : Genode::List<Job>::Element
		{
			char                   name[Builder::MAX_NAME_LEN];
			Genode::Signal_context context;

			Job(char const *job_name)
			{
				Genode::strncpy(name, job_name, sizeof(name));
			}
		};

		Genode::List<Job>        _jobs;
		Genode::Signal_receiver &_sig_rec;
		Builder::Session        &_builder;

	public:

		Jobs(Builder::Session        &builder,
		     Genode::Signal_receiver &receiver)
		: _sig_rec(receiver), _builder(builder) { }

		~Jobs()
		{
			for (Job *job = _jobs.first(); job; job = job->next()) {
				_sig_rec.dissolve(&job->context);
				destroy(Genode::env()->heap(), job);
			}		
		}

		/**
		 * Return a pointer to a job name, or zero.
		 */
		char const *lookup_context(Genode::Signal_context *context)
		{
			for (Job *job = _jobs.first(); job; job = job->next())
				if (&job->context == context)
					return job->name;
			return 0;
		}

		/**
		 * Add a new job to the builder.
		 *
		 * \throw  Builder::Invalid_derivation
		 */
		void *add(char const *name)
		{
			Job *job = new (Genode::env()->heap()) Job(name);
			_jobs.insert(job);
			_builder.realize(name, _sig_rec.manage(&job->context));
		}

		bool queued(char const *name)
		{
			for (Job *job = _jobs.first(); job; job = job->next())
				if (!Genode::strcmp(job->name, name, Builder::MAX_NAME_LEN))
					return true;
			return false;
		}

		/**
		 * Drop a job from the builder.
		 */
		void *drop(char const *name)
		{
			/* TODO: you need to do something about these slashes */
			while (*name == '/') ++name;

			for (Job *job = _jobs.first(); job; job = job->next())
				if (!Genode::strcmp(job->name, name, Builder::MAX_NAME_LEN)) {
					_sig_rec.dissolve(&job->context);
					_jobs.remove(job);
					destroy(Genode::env()->heap(), job);
					break;
				}
		}

};

#endif