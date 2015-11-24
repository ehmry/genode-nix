with builtins;

let

  nouxDerivation = import ./noux.nix;

  tools = [ "bash" "coreutils" "make" ];

  toolSet = listToAttrs (map
    (name: { inherit name; value = /noux-pkg + "/${name}"; })
    tools
  );

  toolRoots = toString (map (t: ''<fs root="${t}"/>'') tools);

  romSet = listToAttrs (map
    (name: { inherit name; value = getRom name; })
    [ "gmp.lib.so" "mpc.lib.so" "mpfr.lib.so" "stdcxx.lib.so" ]
  );

in

{ platform, toolPrefix }:

nouxDerivation ( toolSet // romSet // {
  verbose = true;
  name = platform;
  builder = "/usr/bin/bash";
  args = [ "/rom/script.sh" ];
  env = { PATH = "/usr/bin"; };

  "script.sh" = builtins.toFile
    "script.sh"
    ''
      /genode/tool/create_builddir ${platform} BUILD_DIR=/out
      echo "CROSS_DEV_PREFIX=${toolPrefix}" > /out/etc/tools.conf
    '';

  genode = /genode;

  fstab =
    ''
      <fs/>
      <dir name="genode"> <fs root="genode"/> </dir>
      <dir name="usr">
        ${toolRoots}
      </dir>
    '';
})
