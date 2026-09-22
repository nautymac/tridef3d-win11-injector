"""Entry point for the Tridef3D_Play_SR build: for SR (Simulated Reality /
SpatialLabs) lenticular panels. Same launcher, plus SRWeaveDX9.dll or
SRWeaveDX11.dll (from the SRCapture3D project, in sr\\x86 and sr\\x64 next
to the exe) is injected after TriDef's DLLs so TriDef's side-by-side output
is woven for the panel inside the game process. Equivalent to
`Tridef3D_Play.exe --sr`."""
import play3d

if __name__ == '__main__':
    play3d.main(default_sr=True)
