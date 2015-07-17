noux:

{ name
, builder
, tarballs
, env  ? { }
, args ? [ ]
}:

with builtins;

let
  strMap = x: y: toString (map x y);
  tarList = map
    (value: { name = baseNameOf (toString value); inherit value; })
    tarballs;
in

derivation ((listToAttrs tarList) // {
  inherit name;
  builder = noux;
  system = currentSystem;
  config = toFile
    "config"
    ''
      <config>
      	<fstab> ${strMap (t: "<tar name=\"${t.name}\"/>") tarList} <fs/> </fstab>
      	<start name="${builder}">
      		${strMap (name: "<env name=\"${name}\" value=\"${getAttr name env}\"/>") (attrNames env)}
      		${strMap (a: "<arg value=\"${a}\"/>") args}
      	</start>
      </config>
    '';
})

