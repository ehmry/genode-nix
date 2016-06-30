/*
 * \brief  Native fetchurl utility for Nix
 * \author Emery Hemingway
 * \date   2016-03-08
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <vfs/dir_file_system.h>
#include <timer_session/connection.h>
#include <os/path.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/heap.h>
#include <base/log.h>

/* cURL includes */
#include <curl/curl.h>

static Genode::Xml_node vfs_config(Genode::Xml_node node)
{
	try { return node.sub_node("vfs"); }
	catch (...) {
		Genode::error("VFS configuration not defined");
		return Genode::Xml_node("<vfs/>");
	}
}

static size_t write_callback(char   *ptr,
                             size_t  size,
                             size_t  nmemb,
                             void   *userdata)
{
	Vfs::Vfs_handle *handle = (Vfs::Vfs_handle*)userdata;

	Vfs::file_size out;
	handle->fs().write(handle, ptr, size*nmemb, out);
	handle->advance_seek(out);
	return out;
}

void Component::construct(Genode::Env &env)
{
	Genode::Attached_rom_dataspace config(env, "config");
	Genode::Heap heap(env.ram(), env.rm());

	Genode::String<256> url;
	Genode::Path<256>   path;
	CURLcode res = CURLE_FAILED_INIT;

	curl_global_init(CURL_GLOBAL_DEFAULT);

	/* wait for DHCP */
	Timer::Connection timer;
	timer.msleep(4000);

	Genode::Xml_node config_node = config.xml();

	Vfs::Dir_file_system vfs(env, heap, vfs_config(config_node),
	                         Vfs::global_file_system_factory());

	bool verbose = config_node.attribute_value("verbose", false);

	config_node.for_each_sub_node("fetch", [&] (Genode::Xml_node node) {
		using namespace Vfs;

		if (res == CURLE_OK) return;
		try {
			node.attribute("url").value(&url);
			node.attribute("path").value(path.base(), path.capacity());
		} catch (...) { Genode::error("error reading 'fetch' node"); return; }

		char const *out_path = path.base();

		Vfs_handle *handle;
		unsigned mode = Directory_service::OPEN_MODE_WRONLY |
			((vfs.leaf_path(out_path)) ?
				0 : Directory_service::OPEN_MODE_CREATE);

		typedef Directory_service::Open_result Result;
		switch (vfs.open(out_path, mode, &handle, heap)) {
		case Result::OPEN_OK: break;

		case Result::OPEN_ERR_UNACCESSIBLE:
			Genode::error(out_path , ": unavailable");
			res = CURLE_WRITE_ERROR; return;

		case Result::OPEN_ERR_NO_PERM:
			Genode::error(out_path, ": permission denied");
			res = CURLE_WRITE_ERROR; return;

		case Result::OPEN_ERR_EXISTS:
			Genode::error(out_path, ": path exists");
			res = CURLE_WRITE_ERROR; return;

		case Result::OPEN_ERR_NAME_TOO_LONG:
			Genode::error(out_path, ": name too long");
			res = CURLE_WRITE_ERROR; return;

		case Result::OPEN_ERR_NO_SPACE:
			Genode::error(out_path, ": no space");
			res = CURLE_WRITE_ERROR; return;
		}

		Vfs_handle::Guard guard(handle);

		CURL *curl = curl_easy_init();
		if (!curl) {
			Genode::error("failed to initialize libcurl");
			res = CURLE_FAILED_INIT;
			return;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.string());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

		curl_easy_setopt(curl, CURLOPT_VERBOSE, verbose);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, true);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, handle);


		/* SSL sucked anyway */
		PWRN("SSL certificate not verified");
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

		res = curl_easy_perform(curl);
		if (res != CURLE_OK)
			Genode::error(curl_easy_strerror(res));

		curl_easy_cleanup(curl);
	});

	curl_global_cleanup();

	env.parent().exit(res ^ CURLE_OK);
}
