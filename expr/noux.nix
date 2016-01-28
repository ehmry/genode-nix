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
, verbose ? false
, passthru ? { }
, ...
} @ attrs:
let
  attrs' = removeAttrs attrs [ "fstab" "env" "args" "passthru" ];
  env' = strMap
    (name: ''<env name="${name}" value="${toString (getAttr name env)}"/>'')
    (attrNames env);
  args' = strMap (arg: ''<arg value="${arg}"/>'') args;
  romSet = listToAttrs (map
    (name: { inherit name; value = getRom name; })
    [ "ld.lib.so" "libc.lib.so" "libm.lib.so" "libc_noux.lib.so" "vfs_any-rom.lib.so" ]
  );
in
(derivation (attrs' // romSet // {
  inherit name;
  builder = getRom "noux";
  system = currentSystem;
  config = toFile
    "config"
    ''
    <config verbose="${if verbose then "yes" else "no"}" stdin="/dev/null" stdout="/dev/log" stderr="/dev/log">
      <fstab>
        ${fstab}
        <dir name="rom"> <any-rom/> </dir>
        <dir name="dev"> <log/> <null/> </dir>
      </fstab>
      <start name="${attrs.builder}">
        ${env'}
        ${args'}
      </start>
    </config>
  '';
})) // passthru
