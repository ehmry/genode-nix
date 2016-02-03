rec {

  ##
  # Replace substring a with substring b in string s.
  replaceInString =
  a: b: s:
  let
    al = builtins.stringLength a;
    bl = builtins.stringLength b;
    sl = builtins.stringLength s;
  in
  if al == 0 then s else
  if sl == 0 then "" else
  if ((builtins.substring 0 al s) == a) then
    b+(replaceInString a b (builtins.substring al sl s))
  else
    (builtins.substring 0 1 s) + (replaceInString a b (builtins.substring 1 sl s));

  ##
  # Determine if a string has the given suffix.
  hasSuffix = suf: str:
    let
      strL = builtins.stringLength str;
      sufL = builtins.stringLength suf;
    in
    if builtins.lessThan strL sufL then false else
      builtins.substring (builtins.sub strL sufL) strL str == suf;

  isLib = name: (hasSuffix ".lib.a" name) || (hasSuffix ".lib.so" name);

  entryArgs =
    { repos = builtins.listToAttrs (map
        (r: { name = builtins.baseNameOf (toString r); value = r; })
        [ /genode/repos/base
          /genode/repos/base-nova
          /genode/repos/os
        ]);
      specs = [ "genode" "nova_x86_64" "acpi" ];
      verbose = true;
    };

  tools  = import ./tools.nix;
  nouxMk = import ./noux.nix;


  #
  # Generate library metadata
  #
  libMeta = lib: import ((toString <store>) + (libMetaDrv lib));
  libMetaDrv = lib:
  nouxMk (entryArgs // {
    name = "${lib}-deps.nix";
    builder = "/bin/make";

    env = { GATHER_DEPS = true; LIB = lib; };
    bash = tools.set.bash;
    make = tools.set.make;
    makefile = ./lib-meta.mk;
    fstab =
      ''
        <dir name="ingest"><fs/></dir>
        <fs root="bash"/>
        <fs root="make"/>
        <rom name="makefile"/>
      '';
  });


  #
  # Build a library
  #
  mkLib = name:
  let
    meta = libMeta name;
    deps = builtins.listToAttrs (map
      (dep: rec { value = mkLib dep; name = value.target; })
      meta.deps);

    target =
      if builtins.hasAttr "sharedLib" meta && meta.sharedLib
      then "${name}.lib.so" else "${name}.lib.a";
  in
  nouxMk (entryArgs // tools.set // tools.romSet // deps // {
    inherit name target;
    builder = "/noux-pkg/bin/make";
    args = [ "-C" "/build" "/ingest/${target}" ];
    outputs = [ target ];

    env =
      { CROSS_DEV_PREFIX = "/noux-pkg/bin/genode-x86-";
        EXTERNAL_OBJECTS = builtins.attrNames deps;
        LIB = name;
        PATH = "/noux-pkg/bin";
      };

    makefile = ./lib.mk;

    fstab =
      ''
        <dir name="ingest"><fs/></dir>
        <dir name="build">
          <rom name="makefile"/>
          <ram/>
        </dir>
        <dir name="noux-pkg"> ${tools.fsRoots} </dir>
        <symlink name="bin" target="/noux-pkg/bin"/>
      '';

     passthru = { inherit deps; };
  });


  #
  # Generate library dependencies for a binary
  #
  binDeps = bin: import ((toString <store>) + (binDepsDrv bin));
  binDepsDrv = bin:
  nouxMk (entryArgs // {
    name = "${bin}-deps.nix";
    builder = "/bin/make";

    env = { GATHER_DEPS = true; BIN = bin; };
    bash = tools.set.bash;
    make = tools.set.make;
    makefile = ./bin-meta.mk;
    fstab =
      ''
        <dir name="ingest"><fs/></dir>
        <fs root="bash"/>
        <fs root="make"/>
        <rom name="makefile"/>
      '';
  });


  #
  # Build a binary
  #
  mkBin = target:
  let
    deps = builtins.listToAttrs (map
      (name: { inherit name; value = mkLib name; })
      (binDeps target));

    name = replaceInString "/" "-" target;
  in
  nouxMk (entryArgs // tools.set // tools.romSet // deps // {
    inherit name;
    builder = "/noux-pkg/bin/make";
    args = [ "-C" "/build" "/ingest/${name}" ];
    outputs = [ name ];

    env =
      { CROSS_DEV_PREFIX = "/noux-pkg/bin/genode-x86-";
        EXTERNAL_OBJECTS = builtins.attrNames deps;
        PATH   = "/noux-pkg/bin";
        TARGET = target;
        NAME   = name;
      };

    makefile = ./bin.mk;

    fstab =
      ''
        <dir name="ingest"><fs/></dir>
        <dir name="build">
          <rom name="makefile"/>
          <ram/>
        </dir>
        <dir name="noux-pkg"> ${tools.fsRoots} </dir>
        <symlink name="bin" target="/noux-pkg/bin"/>
      '';

     passthru = { inherit deps; };
  });
}
