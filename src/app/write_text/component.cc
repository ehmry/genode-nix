/*
 * \brief  Utility to write text
 * \author Emery Hemingway
 * \date   2016-06-22
 *
 * This utility is used to accumulate Nix dependencies for a text file.
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <file_system_session/connection.h>
#include <file_system/util.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/allocator_avl.h>
#include <base/heap.h>
#include <base/log.h>

void Component::construct(Genode::Env &env)
{
	Genode::Attached_rom_dataspace config_rom(env, "config");
	Genode::warning(config_rom.local_addr<char const>());

	Genode::Xml_node const config_node = config_rom.xml();

	Genode::Heap heap(env.ram(), env.rm());
	Genode::Allocator_avl fs_tx_block_alloc(&heap);
	File_system::Connection fs(env, fs_tx_block_alloc, "ingest");

	using namespace File_system;

	Genode::Xml_node const text_node = config_node.sub_node("text");
	size_t const len = text_node.content_size();

	Dir_handle root_handle = fs.dir("/", false);
	File_handle file_handle = fs.file(root_handle, "out", WRITE_ONLY, true);

	fs.truncate(file_handle, len);
	Genode::size_t r = write(fs, file_handle, text_node.content_base(), len);
	fs.close(file_handle);
	env.parent().exit(len-r);
}