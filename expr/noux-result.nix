with builtins;

let
  strMap = x: y: toString (map x y);

  nouxEnv =
  rec {
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
        builder = getRom "noux";
        system = currentSystem;
        config = toFile
          "config"
          ''
            <config verbose="${toString verbose}" stdin="/null" stdout="/log" stderr="/log">
              <fstab>
                ${fstab}
                <log/>
                <null/>
              </fstab>
              <start name="${builder}">
                ${env'}
                ${args'}
              </start>
            </config>
          '';
    })) // passthru;
  };

  getRoms = roms:
  builtins.listToAttrs (map
    (name: { inherit name; value = getRom name; })
    roms
  );

in

  nouxEnv.mkDerivation (
    getRoms
      [ "ld.lib.so"
        "libc.lib.so"
        "libc_noux.lib.so"
        "libm.lib.so"
      ] 
  // {
    name = "touch-test";
    builder = "/bin/bash";
    verbose = true;
    args = [ "/script.sh" ];
    bash = /noux-pkg/bash;
    coreutils = /noux-pkg/coreutils;
    fstab =
      ''
        <fs/>
        <fs root="bash"/>
        <fs root="coreutils"/>
        <inline name="script.sh">
        echo hello
        /bin/mkdir -v /out
        /bin/bash --version > /out/bash-version
        /bin/mkdir --version > /out/mkdir-version
        echo goodbye
        </inline>
      '';
  })
