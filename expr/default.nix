with builtins;

let
  strMap = x: y: toString (map x y);

  nouxEnv = noux:
  rec {
    simpleXmlNode = type: name: ''<${type} name="${name}"/>'';

    tarNode = simpleXmlNode "tar";
    romNode = simpleXmlNode "rom";

    mkDerivation =
      { name
      , builder
      , fstab
      , env  ? { }
      , args ? [ ]
      , verbose ? false
      , passthru ? { }
      , ...
      } @ attrs:
      let
        attrs' = removeAttrs attrs [ "fstab" "env" "args" "passthru" ];
        env' = strMap
          (name: ''<env name="${name}" value="${getAttr name env}"/>'')
          (attrNames env);
        args' = strMap (arg: ''<arg value="${arg}"/>'') args;
      in
      (derivation ((builtins.trace attrs' attrs') // {
        inherit name;
        builder = noux;
        system = currentSystem;
        config = toFile
          "config"
          ''
            <config verbose="${if verbose then "yes" else "no"}">
              <fstab>${fstab}</fstab>
              <start name="${builder}">
                ${env'}
                ${args'}
              </start>
            </config>
          '';
    })) // passthru;
  };

  # Prime nouxEnv with the Noux binary.
  nouxEnv' = nouxEnv /noux;

  tools = map
    (name: /noux-pkgs + "/${name}")
    [ "bash" "coreutils" "make" "which"
      "findutils" "grep" "sed"
    ];

  genode =
  { specs, verbose ? false }:
  name:

  let
    toolRoots = toString (map (t: ''<fs root="${t}"/>'') tools);
  in
  nouxEnv'.mkDerivation {
    inherit name verbose;
    builder = "/usr/bin/make";
    args =
      [ "-C" "/build" name
      ] ++ (if verbose then [ "VERBOSE=" ] else []);
    env =
      { inherit name;
        NOUX_CWD = "/build";
        PATH     = "/usr/bin:/usr/local/genode-gcc/bin";
      };

    fstab =
      ''
        <fs/>
        <dir name="bin">
          <symlink name="sh" target="/usr/bin/bash"/>
        </dir>
        <dir name="genode"> <fs root="${/genode}"/> </dir>
        <dir name="usr">
          ${toolRoots}
          <dir name="local">
            <dir name="genode-gcc">
              <fs root="${/noux-pkgs/gcc_x86}"/>
              <fs root="${/noux-pkgs/binutils_x86}"/>
            </dir>
          </dir>
        </dir>

        <dir name="build">
          <ram/>
          <dir name="etc">
            <inline name="build.conf">
GENODE_DIR    = /genode
BASE_DIR      = $(GENODE_DIR)/repos/base
REPOSITORIES  = $(GENODE_DIR)/repos/base-nova
REPOSITORIES += $(GENODE_DIR)/repos/base
CROSS_DEV_PREFIX="genode-x86-"
              </inline>
            <inline name="specs.conf">SPECS = ${toString specs}</inline>
          </dir>
          <symlink name="Makefile" target="/genode/tool/builddir/build.mk"/>
        </dir>

        <dir name="dev"> <null />  </dir>
      '';
  };

  # Prime the genode function.
  genode' = genode {
    specs = [ "genode nova_x86_64" "acpi" ];
    verbose = false;
  };

in
genode' "core"
