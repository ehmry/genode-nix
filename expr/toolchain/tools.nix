#
# Toolchain tool definitions
#

rec {

  list =
    [ "bash" "coreutils" "findutils" "grep" "make" "sed" "which"
      "binutils_x86" "gcc_x86"
    ];

  set = builtins.listToAttrs (map
    (name: { inherit name; value = /noux-pkg + "/${name}"; })
    list
  );

  fsRoots = toString (map (t: ''<fs root="${t}"/>'') list);

  romSet = builtins.listToAttrs (map
    (name: { inherit name; value = builtins.getRom name; })
    [ "gmp.lib.so" "mpc.lib.so" "mpfr.lib.so" "stdcxx.lib.so" ]
  );

}
