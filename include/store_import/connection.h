/*
 * \brief  Connection to store_import
 * \author Emery Hemingway
 * \date   2015-07-24
 *
 * If a call to create a node throws `No_space',
 * upgrade the session RAM quota and try again.
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__STORE_IMPORT__CONNECTION_H_
#define _INCLUDE__STORE_IMPORT__CONNECTION_H_

#include <file_system_session/client.h>
#include <base/connection.h>
#include <base/allocator.h>
#include <util/arg_string.h>


namespace Store_import {
	using namespace File_system;
	struct Connection;
}


class Store_import::Connection : public Genode::Connection<File_system::Session>, public File_system::Session_client
{
	private:

		size_t _session_quota;

		void upgrade()
		{
			enum { ARGBUF_SIZE = 64 };
			char argbuf[ARGBUF_SIZE];

			size_t donation = _session_quota / 2;
			Genode::Arg_string::set_arg(argbuf, sizeof(argbuf),
			                            "ram_quota", donation);
			PWRN("donating %s bytes to import session");
			Genode::env()->parent()->upgrade(cap(), argbuf);
			_session_quota += donation;
		}

	public:

		enum { INITIAL_QUOTA = 4*1024*sizeof(long) };

		/**
		 * Constructor
		 *
		 * Use double the quota because the server hosts a second connection.
		 *
		 * \param tx_buffer_alloc  allocator used for managing the
		 *                         transmission buffer
		 * \param tx_buf_size      size of transmission buffer in bytes
		 */
		Connection(Range_allocator &tx_block_alloc,
		           size_t           tx_buf_size = 256*1024,
		           const char      *label = "import",
		           const char      *root  = "/")
		:
			Genode::Connection<Session>(
				session("ram_quota=%zd, tx_buf_size=%zd, label=\"%s\", root=\"%s\"",
				        INITIAL_QUOTA + tx_buf_size, tx_buf_size, label, root)),
			Session_client(cap(), tx_block_alloc),
			_session_quota(INITIAL_QUOTA) { }

			File_handle file(Dir_handle dir, Name const &name, Mode mode, bool create) override
			{
				try {
					return Session_client::file(dir, name, mode, create);
				} catch (No_space) {
					upgrade();
					return Session_client::file(dir, name, mode, create);
				}
			}

			Symlink_handle symlink(Dir_handle dir, Name const &name, bool create) override
			{
				try {
					return Session_client::symlink(dir, name, create);
				} catch (No_space) {
					upgrade();
					return Session_client::symlink(dir, name, create);
				}
			}

			Dir_handle dir(File_system::Path const &path, bool create) override
			{
				try {
					return Session_client::dir(path, create);
				} catch (No_space) {
					upgrade();
					return Session_client::dir(path, create);
				}
			}
};

#endif /* _INCLUDE__STORE_IMPORT__CONNECTION_H_ */
