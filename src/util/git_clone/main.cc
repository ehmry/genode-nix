/*
 * \brief  Utility to clone Git repositories
 * \author Emery Hemingway
 * \date   2016-03-02
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <os/config.h>
#include <os/path.h>
#include <base/sleep.h>
#include <timer_session/connection.h>
#include <base/log.h>

/* Git includes */
#include <git2.h>
#include <git2/clone.h>


namespace Git_clone {

	typedef Genode::String<256> Url;
	typedef Genode::Path<256>   Path;

};


int fetch_progress(
            const git_transfer_progress *stats,
            void *payload)
{
/*
 * - total_objects: number of objects in the packfile being downloaded
 * - indexed_objects: received objects that have been hashed
 * - received_objects: objects which have been downloaded
 * - local_objects: locally-available objects that have been injected
 *    in order to fix a thin pack.
 * - received_bytes: size of the packfile received up to now
 */

	Genode::log("fetch ",
	     stats->received_objects,"/",
	     stats->indexed_objects,"/",
	     stats->total_objects," objects - ",
	     stats->received_bytes," bytes recieved");
	return 0;
}


void checkout_progress(
            const char *path,
            size_t cur,
            size_t tot,
            void *payload)
{
	Genode::log("checkout ", path, " ", cur, "/", tot);
}


int main(void)
{
	using namespace Git_clone;

	int error = 0;

	git_libgit2_init();

	Timer::Connection timer;
	timer.msleep(8000);

	Genode::Xml_node config_node = Genode::config()->xml_node();
	bool verbose = config_node.attribute_value("verbose", false);

	Url  url;
	Path path;
	config_node.for_each_sub_node("repo", [&] (Genode::Xml_node repo_node) {
		if (error) return;
		try {
			git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;

			git_repository *repo = NULL;

			repo_node.attribute("url").value(&url);
			repo_node.attribute("path").value(path.base(), path.capacity());

			clone_opts.checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE;

			if (verbose) {
				clone_opts.checkout_opts.progress_cb = checkout_progress;
				clone_opts.fetch_opts.callbacks.transfer_progress = fetch_progress;
			}

			Genode::log("Cloning `", url.string(), "' into `", path.base(), "'");

			error = git_clone(&repo, url.string(), path.base(), &clone_opts);
			if (error < 0) {
				const git_error *e = giterr_last();
				Genode::error("Error ", error, "/", e->klass, ":", e->message);
			}
			git_repository_free(repo);

		} catch (...) { }
	});

	git_libgit2_shutdown();

	return error;
}