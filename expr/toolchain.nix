with builtins;

let

  strMap = x: y: toString (map x y);

  nouxDerivation = import ./noux.nix;

  tools = [ "bash" "coreutils" "make" "which" "findutils" "grep" "sed" ];

  toolSet = listToAttrs (map
    (name: { inherit name; value = /noux-pkg + "/${name}"; })
    tools     
  );

  toolRoots = toString (map (t: ''<fs root="${t}"/>'') tools);

  romSet = listToAttrs (map
    (name: { inherit name; value = getRom name; })
    [ "gmp.lib.so" "mpc.lib.so" "mpfr.lib.so" "stdcxx.lib.so" ]
  );

  ##
  # Replace substring a with substring b in string s.
  replaceInString =
  a: b: s:
  let
    al = stringLength a;
    bl = stringLength b;
    sl = stringLength s;
  in
  if al == 0 then s else
  if sl == 0 then "" else
  if ((substring 0 al s) == a) then
    b+(replaceInString a b (substring al sl s))
  else
    (substring 0 1 s) + (replaceInString a b (substring 1 sl s));

in
{ specs, verbose ? false }:
target:
nouxDerivation ( toolSet // romSet // {
  inherit verbose;
  name = replaceInString "/" "-" target;
  builder = "/usr/bin/make";
  args = [ "-C" "/build" target ] ++ (if verbose then [ "VERBOSE=" ] else []);

  env = {
    PATH = "/usr/bin:/usr/local/genode-gcc/bin";
  };

  genode = /genode;
  binutils = /noux-pkg/binutils_x86;
  gcc =      /noux-pkg/gcc_x86;

  buildDir = import ./builddir.nix {
    platform = "nova_x86_32";
    toolPrefix = "genode-x86-";
  };

  fstab =
    ''
      <dir name="genode"> <fs root="genode"/> </dir>
      <dir name="usr">
        ${toolRoots}
        <dir name="local">
          <dir name="genode-gcc">
            <fs root="binutils"/>
            <fs root="gcc"/>
          </dir>
        </dir>
      </dir>
      <fs root="bash"/>
      <symlink name="bin" target="usr/bin"/>

      <dir name="build">
        <fs/>
        <fs root="buildDir"/>
      </dir>
    '';
})
