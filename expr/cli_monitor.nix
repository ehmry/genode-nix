let
  withTerm =
  { help, start
  , fontSize ? null
  , width ? 800, height ? 400
  , ram ? "256M"
  , extraServices ? [ ]
  }: name:
  let
    extraServices' = toString (
      map (x: ''<service name="${x}"/>'') extraServices
    );
  in
  builtins.toFile
    "${name}.subsystem"
    ''
      <subsystem name="${name}" help="${help}">
        <resource name="RAM" quantum="${ram}"/>
        <binary name="init"/>
        <config>
          <parent-provides>
            <service name="ROM"/>
            <service name="LOG"/>
            <service name="CAP"/>
            <service name="RAM"/>
            <service name="RM"/>
            <service name="CPU"/>
            <service name="PD"/>
            <service name="SIGNAL"/>
            <service name="Timer"/>
            <service name="Rtc"/>
            <service name="Builder"/>
            <service name="Nitpicker"/>
            <service name="File_system"/>
            ${extraServices'}
          </parent-provides>
          <default-route>
            <any-service> <child name="terminal"/> <parent/> </any-service>
          </default-route>

          <start name="nit_fb">
            <resource name="RAM" quantum="3M"/>
            <route> <any-service> <parent/> </any-service> </route>
            <provides> <service name="Framebuffer"/>
                       <service name="Input"/> </provides>
            <config width="${toString width}" height="${toString height}"/>
          </start>

          <start name="terminal">
            <resource name="RAM" quantum="2M"/>
            <provides><service name="Terminal"/></provides>
            <route>
              <any-service>
                <child name="nit_fb"/>
                <parent/>
              </any-service>
            </route>
            <config>
              <keyboard layout="dvorak"/>
              <font size="${toString fontSize}"/>
            </config>
          </start>

          ${start}

        </config>
      </subsystem>
    '';

  nouxWithTerm = { help, config, fontSize ? null, width ? 600, height ? 400, ram ? "256M" }:
  withTerm {
    inherit help fontSize width height ram;
    start =
      ''
        <start name="noux">
          <resource name="RAM" quantum="${ram}" />
          <exit propagate="yes"/>
          ${config}
        </start>
      '';
  };

  withNitfb = { binary, width, height, config ? "", ram ? "32M" }:
  name:
  builtins.toFile
    (name+".subsystem")
    ''
      <subsystem name="${name}" help="${name}">
        <resource name="RAM" quantum="${ram}" />
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
          <service name="File_system"/>
      </parent-provides>
      <default-route>
          <any-service> <parent/> <any-child/> </any-service>
      </default-route>

      <start name="nit_fb">
          <resource name="RAM" quantum="8M"/>
          <provides>
            <service name="Framebuffer"/>
            <service name="Input"/>
          </provides>
          <config width="${width}" height="${height}"/>
          <route> <any-service> <parent/> </any-service> </route>
      </start>
      <start name="${name}">
          <binary name="${binary}"/>
          <exit propagate="no"/>
          <resource name="RAM" quantum="${ram}"/>
          ${config}
      </start>
</config>
</subsystem>
'';

  gameSubsystem = { binary, width, height, vfs ? "", RAM ? "32M" }: name:
builtins.toFile
(name+".subsystem")
''
<subsystem name="${name}" help="${name}">
      <resource name="RAM" quantum="${RAM}" />
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
          <service name="Audio_out"/>
          <service name="File_system"/>
      </parent-provides>
      <default-route>
          <any-service> <parent/> <any-child/> </any-service>
      </default-route>

      <start name="nit_fb">
          <resource name="RAM" quantum="8M"/>
          <provides>
            <service name="Framebuffer"/>
            <service name="Input"/>
          </provides>
          <config width="${width}" height="${height}"/>
      </start>
      <start name="${name}">
          <binary name="${binary}"/>
          <exit propagate="yes"/>
          <resource name="RAM" quantum="32M"/>
          <config>
            <sdl_audio_volume value="100"/>
            <libc stdout="/dev/log" stderr="/dev/log">
              <vfs>
                ${vfs}
                <dir name="dev"> <log/> </dir>
              </vfs>
            </libc>
          </config>
      </start>
</config>
</subsystem>
'';

  dosGame = { dosName, width, height, RAM ? "128M", iso ? null }:
  gameSubsystem {
    inherit width height RAM;
    binary = "dosbox";
    vfs =
      ''
        <inline name="dosbox.conf">
        [autoexec]
        @echo off
        ${if iso == null then "" else "IMGMOUNT -t iso d ${iso}"}
        MOUNT C /
        C:
        CD ${dosName}
        ${dosName}
        exit
        </inline>
        <fs/>
      '';
  };

  ##
  # Drop a suffix from the end of a string.
  dropSuffix = suf: str:
    let
      strL = builtins.stringLength str;
      sufL = builtins.stringLength suf;
    in
    if builtins.lessThan strL sufL || builtins.substring (builtins.sub strL sufL) strL str != suf
    then abort "${str} does not have suffix ${suf}"
    else builtins.substring 0 (builtins.sub strL sufL) str;

in
subsystem:
let name = dropSuffix ".subsystem" subsystem; in
(builtins.getAttr name
rec {
  tyrian = gameSubsystem {
    binary = "opentyrian";
    width = "640"; height = "400";
    vfs =
      ''
         <tar name="tyrian.tar"/>
         <fs/>
      '';
    };

  abuse = gameSubsystem rec {
    binary = "abuse";
    width = "640"; height = "400";
    vfs =
      ''
        <inline name="abuserc">datadir=/data</inline>
        <dir name="data">
          <tar name="abuse.tar"/>
          <fs/>
        </dir>
      '';
  };

  hexen2 = gameSubsystem {
    binary = "uhexen2";
    width = "1024"; height="768";
    vfs = "<fs/>";
    RAM = "128M";
  };

  slides = withNitfb {
    binary = "mupdf";
    width = "1000"; height = "744";
    ram = "64M";
    config =
      ''
        <config pdf="talk.pdf">
          <libc stdout="/dev/log" stderr="/dev/log">
            <vfs>
              <fs/>
              <dir name="dev"> <log/> </dir>
            </vfs>
          </libc>
        </config>
      '';
  };

  bash = nouxWithTerm rec {
    help = "Bash shell";
    ram = "64M";
    fontSize = 12;
    width = 800; height = 600;
    config =
      ''
<config>
	<fstab>
		<fs/>

		<dir name="pkg">
			<fs root="bash"      label="noux-pkg"/>
			<fs root="coreutils" label="noux-pkg"/>
			<fs root="vim"       label="noux-pkg"/>
			<fs root="findutils" label="noux-pkg"/>
			<fs root="make"      label="noux-pkg"/>
		</dir>

		<dir name="reports">
			<rom name="wlan_accesspoints"/>
			<rom name="wlan_state"/>
		</dir>
		<dir name="dev"> <zero/>             </dir>
		<dir name="nix"> <fs label="store"/> </dir>
	</fstab>
	<start name="/pkg/bin/bash">
		<env name="PATH" value="/pkg/bin" />
		<env name="TERM" value="linux" />
	</start>
</config>
      '';
  };

  nix-repl = withTerm {
    help = "Nix read-evaluate-print-loop";
    start =
      ''
<start name="nix-repl" priority="-1">
	<resource name="RAM" quantum="16M"/>
	<config>
		<nix verbosity="9">
			<vfs>
				<dir name="expr">
					<fs root="/expr"/>
				</dir>
				<dir name="noux-pkg">
					<fs root="noux-pkg"/>
				</dir>
				<dir name="genode">
					<fs root="genode"/>
				</dir>
				<dir name="nix">
					<dir name="corepkgs">
						<tar name="nix_corepkgs.tar"/>
					</dir>
				</dir>
			</vfs>
		</nix>
	</config>
</start>
      '';
    width = 800; height = 600;
    extraServices = [ "Builder" ];
  };

}) name
