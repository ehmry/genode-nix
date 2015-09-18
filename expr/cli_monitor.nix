with builtins;

{
  logSubsystem = { name, help ? "", ram ? "32M" }:
  builtins.toFile
    (name+".subsystem")
    ''
      <subsystem name="${name}" help="${help}">
        <resource name="RAM" quantum="${ram}"/>
        <binary name="init"/>
        <config>
          <parent-provides>
            <service name="ROM"/>
            <service name="RAM"/>
            <service name="CAP"/>
            <service name="PD"/>
            <service name="RM"/>
            <service name="CPU"/>
            <service name="LOG"/>
            <service name="SIGNAL"/>
            <service name="Timer"/>
            <service name="Nitpicker"/>
         </parent-provides>
          <start name="nitlog">
            <resource name="RAM" quantum="4M"/>
            <provides> <service name="LOG"/> </provides>
			<route>
				<any-service> <parent/> </any-service>
			</route>
          </start>
          <start name="${name}">
            <resource name="RAM" quantum="${ram}"/>
			<route>
              <service name="LOG"> <child name="nitlog"/> </service>
              <any-service> <parent/> </any-service>
			</route>
          </start>
        </config>
      </subsystem>
    '';
}
