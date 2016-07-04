#
# Generic Noux environment
#

with builtins;

let
  strMap = x: y: toString (map x y);
in

{ name
, builder
, fstab
, env  ? { }
, args ? [ ]
, roms ? { }
, parentRoms ? [ ]
, verbose ? false
, passthru ? { }
, ...
} @ attrs:
let
  attrs' = removeAttrs attrs [ "fstab" "env" "args" "passthru" "roms" "parentRoms" ];
  env' = strMap
    (name: ''<env name="${name}" value="${toString (getAttr name env)}"/>'')
    (attrNames env);
  args' = strMap (arg: ''<arg value="${arg}"/>'') args;

  romSet = roms // (listToAttrs (map
    (name: { inherit name; value = getRom name; })
    ( parentRoms ++
      [ "ld.lib.so" "libc.lib.so" "libm.lib.so" "libc_noux.lib.so" ]
    )
  ));
in
(derivation (attrs' // romSet // {
  inherit name;
  builder = getRom "noux";
  system = currentSystem;
  config =
    ''
    <config verbose="${if verbose then "yes" else "no"}" stdin="/dev/null" stdout="/dev/log" stderr="/dev/log">
      <fstab>
        <dir name="dev"> <log/> <null/> </dir>
        ${fstab}
      </fstab>
      <start name="${attrs.builder}">
        ${env'}
        ${args'}
      </start>
    </config>
  '';
})) // passthru
