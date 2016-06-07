#include <hash/blake2s.h>
#include <base/printf.h>
#include <base/attached_rom_dataspace.h>
#include <util/string.h>
#include <base/component.h>
#include <base/log.h>

static char const alph[0x10] = {
	'0','1','2','3','4','5','6','7',
	'8','9','a','b','c','d','e','f'
};

template <unsigned N>
struct Hex_string
{
	Genode::uint8_t const (&value)[N];

	Hex_string(Genode::uint8_t const (&bin)[N]) : value(bin) { }

	void print(Genode::Output &output) const
	{
		for (unsigned i = 0; i < N; ++i) {
			output.out_char(alph[value[i] & 0x0F]);
			output.out_char(alph[value[i] >> 4]);
		}
	}
};


void Component::construct(Genode::Env &env)
{
	int failed = 0;

	using namespace Genode;
	using namespace Hash;

	enum { HASH_SIZE = 32 };

	Hash::Blake2s hash;
	size_t const hex_digest_size = hash.size()*2+1;

	static Attached_rom_dataspace config_rom { env, "config" };
	
	typedef Genode::String<64> Rom_name;

	config_rom.xml().for_each_sub_node("rom", [&] (Genode::Xml_node node) {

		uint8_t digest_buf[HASH_SIZE];

		Rom_name rom_name;
		node.attribute("name").value(&rom_name);
		Attached_rom_dataspace rom(env, rom_name.string());

		hash.reset();
		hash.update(rom.local_addr<uint8_t>(), rom.size());
		hash.digest(digest_buf, hash.size());

		for (int i = hash.size()-1, j = hex_digest_size-2; i >= 0; --i) {
			digest_buf[j--] = alph[digest_buf[i] & 0x0F];
			digest_buf[j--] = alph[digest_buf[i] >> 4];
		}

		if (node.has_attribute("hash")) {
			char check_buf[hex_digest_size];
			node.attribute("hash").value(check_buf, sizeof(check_buf));
			if (strcmp(check_buf, (char*)digest_buf)) {
				error((Hex_string<HASH_SIZE>)digest_buf, ": ", rom_name.string());
				++failed;
				return;
			}
		}

		log((Hex_string<HASH_SIZE>)digest_buf, ": ", rom_name.string());
	});

	env.parent().exit(failed);
}