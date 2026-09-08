## raylib examples
```
.\mc.bat deps

.\mc.bat build raylib

.\build-raylib\examples\Release\microconsole_examples.exe
.\build-raylib\examples\Release\microconsole_examples.exe --example snes-mode7
.\build-raylib\examples\Release\microconsole_examples.exe --list

.\build-raylib\examples\Release\microconsole_examples.exe --volume 50
.\build-raylib\examples\Release\microconsole_examples.exe --example neogeo-zoom --volume 50

.\build-raylib\examples\Release\mc_example_capture.exe --quiet

.\build-raylib\examples\Release\mc_example_capture.exe `
    --example snes-mode7 `
    --frames 240 `
    --shot 60 `
    --shot 180
```

## DOS examples
```
.\mc.bat build dos-examples

.\mc.bat run dos-examples

.\mc.bat run dos-examples /list
.\mc.bat run dos-examples /example snes-mode7
.\mc.bat run dos-examples /example a2600-beam
.\mc.bat run dos-examples /example nes-split
.\mc.bat run dos-examples /example sms-lock
.\mc.bat run dos-examples /example gb-window
.\mc.bat run dos-examples /example pce-wavetable
.\mc.bat run dos-examples /example md-linescroll
.\mc.bat run dos-examples /example snes-colormath
.\mc.bat run dos-examples /example neogeo-zoom

.\mc.bat run dos-examples /example nes-split /frames 400
.\mc.bat run dos-examples /example nes-split /frames 400 /noaudio
.\mc.bat run dos-examples /example snes-mode7 /volume 50
```

## Pico Plus 2 port
```
.\mc.bat build pico max98357a

python scripts\mc_pico.py flash-examples max98357a swd

python scripts\mc_pico.py flash-examples max98357a picotool

python scripts\mc_pico.py flash-examples max98357a manual

python scripts\mc_pico.py list-examples

python scripts\mc_pico.py example a2600-beam
python scripts\mc_pico.py example nes-split
python scripts\mc_pico.py example sms-lock
python scripts\mc_pico.py example snes-mode7
python scripts\mc_pico.py example gb-window
python scripts\mc_pico.py example pce-wavetable
python scripts\mc_pico.py example md-linescroll
python scripts\mc_pico.py example snes-colormath
python scripts\mc_pico.py example neogeo-zoom

python scripts\mc_pico.py command NEXT
python scripts\mc_pico.py command PREV
```

#### The Pico Plus 2's BOOT/user button also cycles to the next example.
```
python scripts\mc_pico.py volume 50
python scripts\mc_pico.py command SFX

python scripts\mc_pico.py command ACTION
python scripts\mc_pico.py command DEBUG

python scripts\mc_pico.py command "KEY LEFT DOWN"
python scripts\mc_pico.py command "KEY LEFT UP"

python scripts\mc_pico.py command "KEY RIGHT DOWN"
python scripts\mc_pico.py command "KEY RIGHT UP"

python scripts\mc_pico.py command "KEY UP DOWN"
python scripts\mc_pico.py command "KEY UP UP"

python scripts\mc_pico.py command "KEY DOWN DOWN"
python scripts\mc_pico.py command "KEY DOWN UP"
```

