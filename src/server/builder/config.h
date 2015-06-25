/*
 * \brief  Convert Aterm trees to XML configs
 * \author Emery Hemingway
 * \date   2015-06-17
 */

#ifndef _BUILDER__CONFIG_H_
#define _BUILDER__CONFIG_H_

/* Genode includes */
#include <util/xml_generator.h>
#include <util/xml_node.h>
#include <rom_session/connection.h>

/* Local includes */
#include "aterm_parser.h"


namespace Builder {

	/*
	void aterm_to_xml_attr(Aterm::Matcher        &aterm,
	                       Genode::Xml_generator &xml)
	{
		aterm.appl("attr");
		xml.attribute(aterm.string(), aterm.string());
	}
	*/

	/**
	 * Read an XML node from the aterm pattern
	 * node(<name>,[<attr(<name>,<value>), ...],[<value>, ...])
	 */
	/*
	void aterm_to_xml_node(Aterm::Matcher        &aterm,
	                       Genode::Xml_generator &xml)
	{
		aterm.appl("node");
		xml.node(aterm.string(), [&aterm, &xml] {
			try { for (;;)
				aterm_to_xml_attr(aterm, xml);
			} catch (Aterm::Mismatch) { }

			try { for(;;)
				aterm_to_xml_node(aterm, xml);
			} catch (Aterm::Mismatch) { }
		});
	}
	*/

	struct Attachment_handle
	{
		Genode::Rm_session &_rm;
		char               *_attachment;

		Attachment_handle(Genode::Rm_session &rm, char *attachment)
		: rm(_rm), _attachment(attachment) { }

		~Attachment_handle()
		{
			_rm.detach(_attachment);
		}
	}

	/**
	 * Generate a config from an Aterm buffer
	 */
	void noux_config(Dataspace_capability config_ds, char const *source)
	{
		char* config_ds_addr = Genode::env()->rm_session()->attach(config_ds);
		Attachment_handle attach_handle(*Genode::env()->rm_session(),
		                                config_ds_addr);

		//TODO take away the capture and see if it works
		Genode::Xml_generator xml(config_ds_addr, len, "config", [&] {

			Aterm::Matcher aterm(source, strlen(source));

			// TODO: get the config application

			auto node_converter = [&aterm, &xml] {
				aterm.appl("node");
				xml.node(aterm.string(), [&aterm, &xml] {

					aterm.list([&aterm, &xml] {
						aterm.appl("attr");
						xml.attribute(aterm.string(), aterm.string());
					});

					aterm.list(node_coverter);
				});

			};

		});

	}

}

#endif