noux:

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
})) // passthru