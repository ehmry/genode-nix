#
# Noux environment
#

let
  nouxEnv = import ./noux.nix; 
  makeTar = builtins.getRom "make.tar";
  toolDir = ../tool;
in

{ name
, makefile
, env ? { }
, parentRoms ? [ ]
, fstab ? ""
, verbose ? false
, passthru ? { }
, ...
} @ attrs:

nouxEnv (attrs // {
  builder = "/bin/make";
  args = [ "-f" makefile ];

  env = env // {
    TOP_DIR = "/";
    VERBOSE = if verbose then "" else "@";
  };

  parentRoms = parentRoms ++ [ "make.tar" "bash.tar" ]; 
  fstab =
    ''
      <tar name="bash.tar"/>
      <tar name="make.tar"/>
      ${fstab}
    '';
})
