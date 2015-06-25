/*
 * \brief  Builder session component
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _BUILDER__STORE_SESSION_H_
#define _BUILDER__STORE_SESSION_H_

/* Genode includes */
#include <file_system_session/connection.h>
#include <file_system/util.h>
#include <base/snprintf.h>
#include <base/affinity.h>
#include <base/allocator_guard.h>
#include <base/printf.h>
#include <base/service.h>
#include <base/signal.h>
#include <ram_session/client.h>
#include <root/component.h>
#include <os/config.h>
#include <os/server.h>
#include <cap_session/cap_session.h>

/* Local includes */
#include "job.h"

namespace Builder { class Session_component; };

class Builder::Session_component : public Genode::Rpc_object<Session>
{
	private:

		Genode::Allocator_guard  _session_alloc;
		Genode::Allocator_avl    _fs_block_alloc;
		File_system::Connection  _store_fs;
		Genode::Lock             _store_fs_lock;
		File_system::Dir_handle  _store_dir;
		Job_queue               &_job_queue;

	public:

		/**
		 * Constructor
		 */
		Session_component(char const          *label,
		                  Server::Entrypoint  &ep,
		                  Allocator           *session_alloc,
		                  size_t               ram_quota,
		                  Job_queue           &job_queue)
		:
			_session_alloc(session_alloc, ram_quota),
			_fs_block_alloc(&_session_alloc),
			_store_fs(_fs_block_alloc, 128*1024, "store"),
			_job_queue(job_queue)
		{
			// TODO donate to queue preservation
			_store_dir = _store_fs.dir("/", false);
		}

		~Session_component()
		{
			// TODO withdraw from job queue
			 _store_fs.close(_store_dir);
		}

		/************************************
		 ** Builder session interface **
		 ************************************/

		void realize(Name const  &drv_name, Genode::Signal_context_capability sigh)
		{
			using namespace File_system;

			if (!drv_name.valid())
				throw Invalid_derivation();

			char const *name = drv_name.string();

			if (File_system::string_contains(name, '/'))
				throw Invalid_derivation();

			if (Job *job = _job_queue.lookup_name(name)) {
				job->sigh(sigh);
				return;
			}

			/* Prevent packet mixups. */
			Genode::Lock::Guard guard(_store_fs_lock);
			collect_acknowledgements(*_store_fs.tx());

			_job_queue.create(&_session_alloc, _store_fs, name, sigh);
			_job_queue.process();
		}

};

#endif /* _BUILDER__STORE_SESSION_H_ */