/*
 * \brief  Nix declarative VFS plugin
 * \author Emery Hemingway
 * \date   2015-09-07
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Nix includes */
#include <attr-path.hh>
#include <shared.hh>
#include <common-opts.hh>
#include <eval.hh>
#include <get-drvs.hh>
#include <store.hh>
#include <util.hh>

/* Genode includes */
#include <builder_session/builder_session.h>
#include <vfs/file_system_factory.h>
#include <vfs/dir_file_system.h>
#include <vfs/rom_file_system.h>
#include <vfs/fs_file_system.h>
#include <os/config.h>

class Nix_factory : public Vfs::File_system_factory
{
	private:

		enum { STORE_LABEL_LEN = 64 };

		/*
		 * XXX: These members minimize evaluation effort, but
		 * don't need to persist after the VFS is created.
		 */
		char                 _store_label[STORE_LABEL_LEN];
		Vfs::Dir_file_system _nix_vfs;
		nix::Vfs_root        _vfs_root;
		nix::Store           _store;
		nix::EvalState       _eval_state;

		nix::Path realise(Genode::Xml_node node)
		{
			using namespace nix;

			char tmp_buf[Vfs::MAX_PATH_LEN] = { '\0' };

			Value root_value;
			Expr *e;


			/****************
			 ** Parse file **
			 ****************/

			try {
				node.attribute("file").value(tmp_buf, sizeof(tmp_buf));
				e = _eval_state.parseExprFromFile(Path(tmp_buf));

			} catch (Genode::Xml_node::Nonexistent_attribute) {
				e = _eval_state.parseExprFromFile("/default.nix");
			}
			_eval_state.eval(e, root_value);


			/***********************
			 ** Collect arguments **
			 ***********************/
			std::map<string, string> arg_map;

			node.for_each_sub_node("arg", [&arg_map] (Genode::Xml_node arg_node) {
				char  name[64] = { '\0' };
				char value[64] = { '\0' };

				arg_node.attribute("name").value(name, sizeof(name));
				arg_node.attribute("value").value(value, sizeof(value));
				arg_map[name] = value;
			});

			/* enclose value in quotes as a convienence*/
			node.for_each_sub_node("argstr", [&arg_map] (Genode::Xml_node arg_node) {
				char  name[64] = { '\0' };
				char value[64] = { '"' };

				arg_node.attribute("name").value(name, sizeof(name));
				arg_node.attribute("value").value(value+1, sizeof(value)-1);
				arg_map[name] = value;
			});

			Bindings &args(*evalAutoArgs(_eval_state, arg_map));


			/********************
			 ** Find attribute **
			 ********************/

			Value *v;
			try {
				node.attribute("attr").value(tmp_buf, sizeof(tmp_buf));
				v = findAlongAttrPath(_eval_state, tmp_buf, args, root_value);
			} catch (Genode::Xml_node::Nonexistent_attribute) {
				v = findAlongAttrPath(_eval_state, "", args, root_value);
			}


			/*************
			 ** realise **
			 *************/

			PathSet context;
			Value result;
			if (args.empty())
				result = *v;
			else
				_eval_state.autoCallFunction(args, *v, result);

			DrvInfo drv_info(_eval_state);
			if (getDerivation(_eval_state, result, drv_info, false)) {
				PathSet drv_set{ drv_info.queryDrvPath() };
				_store.buildPaths(drv_set, nix::bmNormal);

				return drv_info.queryOutPath();
			}

			switch (result.type) {
			case tPath:
				return result.path;
			case tString:
				return Path(result.string.s);
			default:
				PERR("evaluation result is not a string or path");
			}
			return "";
		}

		Vfs::File_system *from_symlink(File_system::Session &fs,
		                               char const *path)
		{
			using namespace ::File_system;

			Dir_handle root = fs.dir("/", false);

			Symlink_handle link = fs.symlink(root, path+1, false);

			/* top-level store symlinks don't contain any slashes */
			char target[Builder::MAX_NAME_LEN] = { '/' };
			if (read(fs, link, &target[1], sizeof(target)-1) == 0) {
				PERR("failed to determine final path of %s", path);
				return nullptr;
			}

			/*
			 * If the output is a directory, return a File_system
			 * session, otherwise, return a ROM session.
			 */
			Node_handle node = fs.node(target);
			Status status = fs.status(node);

			if (status.is_directory())
				return from_directory(target);
			return from_file(target);
		}

		Vfs::File_system *from_directory(char const *path)
		{
			return new (Genode::env()->heap())
					Vfs::Fs_file_system(File_system::DEFAULT_TX_BUF_SIZE,
					                    _store_label, path, false);
		}

		Vfs::File_system *from_file(char const *path)
		{
			while (*path == '/') ++path;

			char const *name = path;
			while (*name) {
				if (*name == '-') {
					++name;
					break;
				}
				++name;
			}

			return new (Genode::env()->heap())
				Vfs::Rom_file_system(name, path, _store_label);
		}

	public:

		/**
		 * Constructor
		 */
		Nix_factory()
		:
			_nix_vfs(
				Genode::config()->xml_node().sub_node("nix").sub_node("vfs"),
				Vfs::global_file_system_factory()),
			_vfs_root(_nix_vfs),
			_store(_vfs_root),
			_eval_state(_vfs_root, _store,
			            Genode::config()->xml_node().sub_node("nix"))
		{
			try {
				Genode::config()->xml_node().sub_node("nix").attribute("store_label").value(
					_store_label, sizeof(_store_label));
			} catch (Genode::Xml_node::Nonexistent_attribute) {
				Genode::strncpy(_store_label, "store", sizeof(_store_label));
			}
		}

		Vfs::File_system *create(Genode::Xml_node node) override
		{
			nix::Path out_path;

			nix::handleExceptions("nix_realize", [&] {
				out_path = realise(node);
			});
			if (out_path == "")
				return nullptr;
			char const *path = out_path.c_str();

			/* return an appropriate file system for the output */
			try {
				using namespace ::File_system;

				Genode::Allocator_avl alloc(env()->heap());
				::File_system::Connection fs(
					alloc, DEFAULT_TX_BUF_SIZE, _store_label);

				/*
				 * The handles we open are closed when
				 * the session is destroyed at return.
				 */

				Node_handle node = fs.node(path);
				Status status = fs.status(node);

				if (status.is_symlink())
					return from_symlink(fs, path);

				if (status.is_directory())
					return from_directory(path);

				return from_file(path);

			} catch (...) {
				PERR("failed to determinal final path of %s", out_path.c_str());
				throw;
			}
		}
};

extern "C" Vfs::File_system_factory *vfs_file_system_factory(void)
{
	static Nix_factory _factory;
	return &_factory;
}
