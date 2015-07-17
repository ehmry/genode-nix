let nouxDerivation = import ./noux-env.nix /rom/noux; in

nouxDerivation {
  name = "echo-test";
  builder = "/bin/bash";
  tarballs = [ /noux-pkgs/bash.tar /noux-pkgs/coreutils.tar ];
  args = [ "-c" "echo $greeting ${builtins.currentSystem} > /out" ];
  env = { greeting = "Guten abend"; };
}
