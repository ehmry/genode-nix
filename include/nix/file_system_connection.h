/*
 * \brief  Convenience Utility to open a File_system session to Nix store
 * \author Emery Hemingway
 * \date   2016-05-29
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__NIX__FILE_SYSTEM_CONNECTION_H_
#define _INCLUDE__NIX__FILE_SYSTEM_CONNECTION_H_

#include <file_system_session/connection.h>

namespace Nix { class File_system_connection; }


struct Nix::File_system_connection : File_system::Connection
{
	File_system_connection(Genode::Env             &env,
	                       Genode::Range_allocator &tx_block_alloc,
	                       char const              *root        = "/",
	                       bool                     writeable   = true,
	                       Genode::size_t           tx_buf_size = File_system::DEFAULT_TX_BUF_SIZE)
	:
		File_system::Connection(env, tx_block_alloc, "store",
		                        root, writeable, tx_buf_size)
	{}
};

#endif /* _INCLUDE__NIX__FILE_SYSTEM_CONNECTION_H_ */
