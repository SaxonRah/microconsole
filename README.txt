MicroConsole r8 DOS runner fix

Replaces:
  scripts\mc_run.bat

This makes MicroConsole DOS use the same DOSBox benchmark environment as standalone MicroRender:
  machine=vgaonly
  core=dynamic
  cycles=max (or MC_DOSBOX_CYCLES override)
  frameskip=0
  aspect=false

No rebuild is required for this runner-only fix.

Test:
  .\mc.bat run dos /sprites 1024 /frames 2100 /noaudio
  .\mc.bat run dos /sprites 1024 /frames 2100
