#
# Run a native Genode component and collect the log
#

{ name, ... } @ args:
let
  # Make a set of ROMs to merge into the derivation scope
  roms = builtins.listToAttrs (map
    (name: { inherit name; value = builtins.getRom name; })
    [ name "fs_log" "timer" ]
  );
in 
derivation (roms // args // {
  system  = builtins.currentSystem;
  builder = builtins.getRom "init";
  outputs = [ (name + ".log") ];

  config = builtins.toFile (name + ".config")
    ''
      <config verbose="yes">
        <parent-provides>
          <service name="CAP"/>
          <service name="File_system"/>
          <service name="LOG"/>
          <service name="RM"/>
          <service name="ROM"/>
          <service name="SIGNAL"/>
          <service name="Timer"/>
        </parent-provides>

        <start name="fs_log">
          <resource name="RAM" quantum="2M"/>
          <provides> <service name="LOG"/> </provides>
          <route>
            <any-service> <parent/> </any-service>
          </route>
          <config verbose="yes"/>
        </start>

        <start name="${name}">
          <exit propagate="yes"/>
          <resource name="RAM" quantum="1M"/>
          <route>
            <service name="LOG"> <child name="fs_log"/> </service>
            <any-service> <parent/> </any-service>
          </route>
        </start>
      </config>
    '';
})
