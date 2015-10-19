target:

let
  nouxDerivation = import ./noux.nix (builtins.getRom "noux");

  pathSet = path: elements:
    builtins.listToAttrs (map
      (name: { inherit name; value = path + "/${name}"; })
      elements
    );

  fstabRoots = x: toString (map (y: ''<fs root="${y}"/>'') x);

  tools =
    [ "bash"
      "coreutils"
      "sed"
      "findutils"
    ];

  toolPaths = pathSet /noux-pkg tools;

  repos = [ "os" ];
  repoPaths = pathSet /genode.git repos; 

  specs = [ "genode" "nova_x86_64" "acpi" ];

  fsDir = name: ''<dir name="${name}"> <fs root="${name}"/> </dir>'';

  buildConf =
    ''
      BASE_DIR = /base
      REPOSITORIES  = /platformBase
      ${toString (map (r: "REPOSITORIES += /${r}"))}
    '';
in

nouxDerivation(toolPaths // repoPaths // rec {
  verbose = true;
  name = target;
  builder = "/usr/bin/make";
  env =
    { inherit target;
      NOUX_CWD = "/build";
      tool_prefix = "genode-x86-";
      platform = "nova_x86_32";
    };

  base = /genode.git/repos/base;
  basePlatform = /genode.git/repos/base-nova;
  tool = /genode.git/tool;

  fstab =
    ''
      <fs/>

      <dir name="usr">
        ${fstabRoots tools}
        <dir name="local"> <dir name="genode-gcc">
          <fs root="${/noux-pkgs/gcc_x86}"/>
          <fs root="${/noux-pkgs/binutils_x86}"/>
        </dir> </dir>
      </dir>

      ${fsDir "base"}
      ${fsDir "basePlatform"}
      ${fsDir "tool"}

      <dir name="build">
        <ram/>
        <dir name="etc">
          <inline name="build.conf"> ${builtins.trace buildConf buildConf} </inline>
          <inline name="specs.conf">SPECS = ${toString specs}</inline>
        </dir>
        <symlink name="Makefile" target="../tool/builddir/build.mk"/>
      </dir>

      <dir name="dev"> <null />  </dir>
    '';
})
