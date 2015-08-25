/*
 * \brief  Builder global configuration
 * \author Emery Hemingway
 * \date   2015-08-25
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _BUILDER__CONFIG_H_
#define _BUILDER__CONFIG_H_

#include <os/config.h>

namespace Builder {

	using namespace Genode;

	struct Service_label
	{
		enum { MAX_LABEL_SIZE = 64 };

		char string[MAX_LABEL_SIZE];

		Service_label(char const *attr, char const *fallback)
		{
			try {
				config()->xml_node().attribute(attr).value(
					string, sizeof(string));
			} catch (...) {
				strncpy(string, fallback, sizeof(string));
			}
		}

	};

	char const *fs_label()
	{
		static Service_label _label("fs_label", "store");
		return _label.string;
	}

	char const *rom_label()
	{
		static Service_label _label("rom_label", "store");
		return _label.string;
	}

};

#endif