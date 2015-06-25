#ifndef _STORE_IMPORT__UTIL_H_
#define _STORE_IMPORT__UTIL_H_


namespace File_system {

	class Session_component;

	inline bool is_root(Path const &path) {
		return (path.size() == 2) && (*path.string() == '/'); }

	/**
	 * Copy the first element of src to dst.
	 * The caller should skip the first '/'.
	 */
	char *first_element(char *dst, char const *src)
	{
		for (int i = 1; i < MAX_NAME_LEN && src[i]; ++i)
			if (src[i] == '/') return Genode::strncpy(dst, src, ++i);
		return Genode::strncpy(dst, src, MAX_NAME_LEN);
	}

	/**
	 * Rewrite src to dst, using new first element parent.
	 */
	char *rewrite_path(char *dst, char const *parent, char const *src)
	{
		size_t parent_len = strlen(parent);
		size_t src_len = strlen(src);

		strncpy(dst, parent, parent_len+1);

		for (size_t i = 1; i < src_len; ++i)
			if (src[i] == '/')
				return strncpy(dst+parent_len, src+i, MAX_PATH_LEN-parent_len);
		return strncpy(dst+parent_len, src, MAX_PATH_LEN-parent_len);
	}

	/**
	 * Write the first element to name and return the start of the second path.
	 */
	char const *split_path(char *name, char const *path)
	{
		for (int i = 1; i < MAX_NAME_LEN && path[i]; ++i)
			if (path[i] == '/') {
				Genode::strncpy(name, path, ++i);
				return path+i;
			}

		size_t len = strlen(path);
		Genode::strncpy(name, path, len+1);
		return path+len;
	}
}

#endif