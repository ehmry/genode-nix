name: text:
derivation {
  inherit name;
  system = builtins.currentSystem;
  builder = builtins.getRom "write_text";
  config = "<config><text>${text}</text></config>";
}
