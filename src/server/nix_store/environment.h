/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__ENVIRONMENT_H_
#define _NIX_STORE__ENVIRONMENT_H_


/* Genode includes */
#include <util/avl_string.h>
#include <util/list.h>

/* Nix includes */
#include <nix_store_session/nix_store_session.h>
#include <nix_store/derivation.h>

/* local includes */
#include "util.h"


namespace Nix_store {

	struct Input;
	struct Inputs;
	struct Mapping;
	struct Environment;
}


struct Nix_store::Input : Genode::Avl_node<Input>
{
	Nix_store::Name const link;
	Nix_store::Name const final;
	Genode::size_t  const len;
	/* store the len to prefix match */

	Input (char const *name, char const *target)
	:
		link(name), final(target),
		len(Genode::strlen(link.string()))
	{ };

	/************************
	 ** Avl node interface **
	 ************************/

	bool higher(Input *i) const {
		return (strcmp(i->link.string(), link.string()) > 0); }

	Input const *lookup(const char *name) const
	{
		if (link == name) return this;

		Input *i = Avl_node<Input>::child(strcmp(name, link.string()) > 0);
		return i ? i->lookup(name) : nullptr;
	}

};


struct Nix_store::Inputs : Genode::Avl_tree<Input>
{
	Genode::Allocator &alloc;

	Inputs(Genode::Env &env, Genode::Allocator &alloc, File_system::Session &fs, Nix_store::Derivation &drv)
	: alloc(alloc)
	{
		using namespace File_system;

		/* read the derivation inputs */
		drv.inputs([&] (Aterm::Parser &parser) {

			Nix_store::Name input;
			parser.string(&input);

			/* load the input dependency */
			Derivation dependency(env, input.string());

			/* roll through the lists of inputs from this dependency */
			parser.list([&] (Aterm::Parser &parser) {

				Name want_id;
				parser.string(&want_id);

				/* roll through the dependency outputs to match the id */
				dependency.outputs([&] (Aterm::Parser &parser) {

					Nix_store::Name id;
					parser.string(&id);

					if (id == want_id) {

						Name input_path;
						parser.string(&input_path);

						// XXX: slash hack
						char const *input_name = input_path.string();
						while (*input_name == '/')
							++input_name;

						Object_path final_path;

						/* dereference the symlink */
						try { final_path = dereference(fs, input_name).string(); }
						catch (File_system::Lookup_failed) {
							Genode::error("missing input symlink ", input_name);
							throw Nix_store::Missing_dependency();
						}

						/* the symlink is resolved */
						insert(new (alloc) Input(input_name, final_path.string()));

					} else {
						parser.string(); /* Path */
					}

					parser.string(); /* Algo */
					parser.string(); /* Hash */
				});
			});
		});

		/* read the source inputs */
		drv.sources([&] (Aterm::Parser &parser) {
			Nix_store::Name source;
			parser.string(&source);

			// XXX: slash hack
			char const *p = source.string();
			while (*p == '/') ++p;
			insert(new (alloc) Input(p, p));
		});
	}

	~Inputs()
	{
		while (Input *input = first()) {
			remove(input);
			destroy(alloc, input);
		}
	}

	Input const *lookup(char const *name) const
	{
		Input const *input = (Input*)first();
		return input ? input->lookup(name) : nullptr;
	}

};


struct Nix_store::Mapping : Genode::Avl_node<Mapping>
{
	Genode::String<File_system::MAX_NAME_LEN> const key;
	Genode::String<File_system::MAX_PATH_LEN> const value;

	Mapping(char const *key_str, char const *value_str)
	: key(key_str), value(value_str) { }

	/************************
	 ** Avl node interface **
	 ************************/

	bool higher(Mapping *m) const {
		return (strcmp(m->key.string(), key.string()) > 0); }

	Mapping const *lookup(const char *key_str) const
	{
		if (key == key_str) return this;

		Mapping *m =
		Avl_node<Mapping>::child(strcmp(key_str, key.string()) > 0);
		return m ? m->lookup(key_str) : 0;
	}

};


struct Nix_store::Environment : Genode::Avl_tree<Mapping>
{
	Genode::Allocator &_alloc;

	/*
	 * XXX: resolve inputs to content addressed paths
	 * Parse the inputs first and resolve the symlinks before
	 * populating the mappings.
	 */

	Environment(Genode::Env           &env,
	            Genode::Allocator     &alloc,
	            File_system::Session  &fs,
	            Nix_store::Derivation &drv,
	            Inputs          const &inputs)
	: _alloc(alloc)
	{
		using namespace File_system;

		typedef Genode::Path<MAX_PATH_LEN>   Path;
		typedef Genode::String<MAX_PATH_LEN> String;
		typedef Genode::String<MAX_NAME_LEN> Name;

		drv.environment([&] (Aterm::Parser &parser) {
			Name   key;
			String value;
			parser.string(&key);
			parser.string(&value);

			Path value_path(value.string());

			bool top_level = value_path.has_single_element();
			while(value.length() > 1 && !value_path.has_single_element())
				value_path.strip_last_element();

			Input const *input = inputs.lookup(value_path.base());
			Mapping *map;

			if (!input) {
				/*
				 * XXX:	this is heavy, remove anything not a path?
				 */
				try { map = new (alloc)
					Mapping(key.string(), dereference(fs, value.string()).string()); }
				catch (File_system::Lookup_failed) { map = new (alloc)
					Mapping(key.string(), value.string()); }

			} else if (top_level) {
				map = new (alloc)
					Mapping(key.string(), input->final.string());

			} else {
				/* rewrite the leading directory */
				Path new_path(value.string()+input->len, input->final.string());
				map = new (alloc)
					Mapping(key.string(), new_path.base());
			}
			insert(map);
		});
	}

	~Environment()
	{
		while(Mapping *map = (Mapping *)first()) {
			remove(map);
			destroy(_alloc, map);
		}
	}

	char const *lookup(char const *key) const
	{
		Mapping const *m = first();
		m = m ? m->lookup(key) : 0;
		return m ? m->value.string() : nullptr;
	}

};

#endif