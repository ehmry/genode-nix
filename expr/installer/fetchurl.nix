{ url ? ""
, urls ? []
, name ? ""
, outputHash ? ""
, outputHashAlgo ? ""
, blake2s ? "", sha256  ? ""
}:

assert builtins.isList urls;
assert (urls == []) != (url == "");

let
  urls' = if urls != [] then urls else [url];
  getRoms = names: builtins.listToAttrs
    (map (name: { inherit name; value = builtins.getRom name; }) names);
in
assert (outputHash != "" && outputHash != "")
  || blake2s != "" || sha256 != "";

derivation (
(getRoms
  [ "ld.lib.so"
    "libc.lib.so"
    "curl.lib.so"
    "lwip.lib.so"
    "libcrypto.lib.so"
    "libssh.lib.so"
    "libssl.lib.so"
    "vfs_jitterentropy.lib.so"
    "zlib.lib.so" ])
//
{
  system = builtins.currentSystem;
  name = if name != "" then name else
    baseNameOf (toString urls');

  builder = builtins.getRom "fetchurl";

  outputHashAlgo = if outputHashAlgo != "" then outputHashAlgo else
    if blake2s != "" then "blake2s" else "sha256";

  outputHash = if outputHash != "" then outputHash else
    if blake2s != "" then blake2s else sha256;

  # No need to double the amount of network traffic
  preferLocalBuild = true;

  impureServices = [ "Nic" ];

  config =
    ''
      <config>
        <libc stdout="/dev/log" stderr="/dev/log">
          <vfs>
            <dir name="dev"> <jitterentropy name="random"/> <log/> </dir>
          </vfs>
        </libc>
        <vfs> <fs label="ingest"/> </vfs>
        ${toString (map (url: ''<fetch url="${url}" path="/out"/>'') urls')}
      </config>
    '';
})
