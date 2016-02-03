#
# Noux toolchain environment
#

{ name
, builder
, repos
, specs
, fstab
, env  ? { }
, args ? [ ]
, verbose ? false
, passthru ? { }
, ...
} @ attrs:

let

  repoNames = builtins.attrNames repos;

  repoDirs = toString (map
    (r: ''<dir name="${r}"><fs root="${r}"/></dir>'')
    repoNames
  );

in
import ../noux.nix ((builtins.removeAttrs attrs [ "repos" "specs"]) // repos // {
  env = env // {
    REPOSITORIES = toString
      (map (r: "/genode/repos/"+r) repoNames);
    SPECS = specs;
    VERBOSE = if verbose then "" else "@";
  };
  fstab =
    ''
      ${fstab}
      <dir name="genode">
        <dir name="repos">${repoDirs}</dir>
      </dir>
    '';
})