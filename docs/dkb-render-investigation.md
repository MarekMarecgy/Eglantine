# DKB Windows rendering investigation (2026-11-02, Codex WIP)

This work is not yet a complete rendering fix. The current changes remove the
HMMA rejection, but gameplay still loses the 3D scene after the first frames.

**Koniec sesji na prośbę użytkownika:** docked nadal daje czarny obraz z HUD-em.
Handheld przywraca scenę, ale widok na otwarty świat ma duże spadki FPS.
Nie uznawać tej gałęzi za zakończoną naprawę. Punkt wznowienia jest na końcu dokumentu.

## Save/profile recovery

Claude's packaged Windows application had redirected `%APPDATA%/eden` into
`%LOCALAPPDATA%/Packages/Claude_pzs8sxrjxfjjc/LocalCache/Roaming/eden`.
Its actual profile is `A166B1F921731151A4939F326E151075`. The ordinary executable
had used profile `7572796B9288615F94BD29D64EAF613A` instead, explaining the apparent
loss of progress and separate caches. Both profiles and the original Claude data
are preserved. The Claude profile/save was copied into the ordinary NAND, its
200-byte profile record appended in slot 1, and `current_user=1` selected.
All three imported save files were SHA-256 verified against the originals.

The orphan `2146CEF84445E0403A7CF11AB5A0312D` contained only `config.sav` and no
progress. It was moved, with hash verification, into the diagnostic backup.
Evidence/backups are in `build/purple-investigation/` (ignored by Git).

## Shader work and passed checks

The Vulkan backend now has an FP32/register-shuffle HMMA.16816.F16 implementation.
It unpacks packed half inputs, computes all four lane outputs with FMA, and rounds
back to half at each guest HMMA boundary. It needs neither native Float16 math,
cooperative matrices, additional shared memory nor added barriers. The existing
Float16 driver workaround stays intact. Eligible Vulkan devices use this path by
default; `EDEN_NX2_TENSOR=off` disables it. `fast` retains the guarded NV path for
straight-line shaders and can use shuffles when cooperative admission fails.

- GUI build: `ninja -C build yuzu` passed (MSVC Release).
- `[tensor],[nx2_video]`: 18 cases, 65,101 assertions passed.
- Generated arithmetic/admission modules passed `spirv-val --target-env vulkan1.2`.
- Actual DKB tensor modules passed Vulkan 1.3 SPIR-V validation with scalar layout.
- `tools/nx2/tensor_gpu_probe.cpp` dispatched the real backend-generated test
  modules on RTX 5070 Ti. 98,304 half output values matched an independent CPU
  matrix reference bit-for-bit, including two chained HMMAs and a conditional
  executed by one subgroup while the other remained inactive.

## Runtime evidence and remaining fault

Original EXE, recovered save: reproducible rejection of native programs
`23ad32f618fdd979` (192 HMMAs) and `18c7fbbd5e6f9cb8` (144 HMMAs), at offsets
`0x2800` and `0x1d60`. This leaves the purple background and HUD.

New EXE: no tensor rejection. Initially the 1920x1080 input scene is valid and the
2880x1620 tensor output contains the scene. Later, when gameplay is active and the
input changes to 2095x1179 (RT width 2096), the input texture is already black
apart from a world-space yellow marker. The upscaler correspondingly outputs
black. This rules out attributing the continuing black screen solely to HMMA.

Input/output are B10G11R11_FLOAT, handles at cbuf0+0x60 and +0x70 (the old fallback
looked at +0x90/+0xa0). The only SSBO is the nonempty 12,288-byte weights buffer at
cbuf0+0x50. Decoded PNGs, TIC descriptors, input/output memory, SPIR-V and IR are in
`build/purple-investigation/image-capture/`.

Diagnostic runs that did not fix loss of scene:

- `EDEN_NX2_SHADER_REUSE=0` together with `EDEN_NX2_ASYNC_TRANSLATION=0`.
- `EDEN_NX2_MME_JIT=0`, `EDEN_NX2_MME_MEMO=0`, `EDEN_NX2_SKIP_SAME=0`.

An opt-in live draw probe is temporarily in `vk_rasterizer.cpp`:
`EDEN_NX2_DRAW_PROBE=<text file>` reads an integer once per frame. Bit 0 disables
depth/stencil tests/writes, bit 1 disables face culling. Zero restores the guest
states on each draw. This diagnostic must be removed before claiming completion.
Mode 128 is passive (no state overrides). The probe sidecar records the actual
`Settings::IsDockedMode()`, independently of the button's delayed label. Controlled
comparison at the same camera confirmed: docked=1 is black with HUD; docked=0
restores the complete scene. Reapplying all guest depth/stencil/cull states (mode 0)
and disabling culling (mode 2) did not fix docked. Earlier apparent improvements
coincided with the user switching to handheld and must not be counted as fixes.
Bit 65536 now triggers a single frame of synchronous fullscreen/postpass texture
captures into `<probe file>-captures`; this is temporary diagnostic code as well.
`EDEN_NX2_TENSOR_DUMP=<directory>` currently enables expensive synchronous tensor
image/IR captures; remove the temporary capture code or keep it clearly opt-in.

## Separate freeze

The initial run froze at a tutorial loop with `SetTerminateResult 2162-0001` and
`Userspace PANIC`. The previous Claude fix addressed 2111-3604, not this abort.
Retests used `EDEN_NX2_VDEC_EOS_WAIT=1`; no claim of loop stability yet.

## Input setting restored at session end

For autonomous repro the normal config's player-0 A button was temporarily set
to keyboard C. The diagnostic process was stopped and the original SDL controller
mapping restored. The original complete INI line is saved in
`build/purple-investigation/original-button-a.txt`. No unrelated settings were
overwritten. The selected Claude profile remains `current_user=1`.

## Handoff — dokładny stan na koniec sesji

Użytkownik zakończył dalsze testy i poprosił o dokumentację, commit oraz push.
Nie wykonywać kolejnych prób w ramach tej zakończonej sesji.

### Ostatnia próba i materiały

- Ostatni build GUI przeszedł: `build/purple-investigation/build-post-capture2.log`.
  Proces diagnostyczny zamknięto; nie pozostawiono uruchomionej gry.
- Kontrolowane przełączenie docked/handheld jest w `mode-comparison-runtime.log`.
  Ostatnie `docked=0` w `draw-probe.txt.status` dotyczy ostatniej próby handheld,
  nie przeczy potwierdzonemu błędowi docked ani ostatniemu zgłoszeniu użytkownika.
- `final-runtime.log` to log ostatniego uruchomienia.
- `draw-probe.txt-captures/` zawiera 28 unikalnych przebiegów z jednej klatki
  **handheld**: RT, duże tekstury wejściowe, TIC, bufory stałe fragment shadera,
  adresy VS/PS i metadane. Odpowiadającego mu zrzutu docked **jeszcze nie wykonano**.
- `first-depth-captures/` to źle odfiltrowana pierwsza próba: 64 głównie przebiegi
  głębokości z formatem RT=0. Nie używać jej do porównania postprocessingu.
- `inspect_posts.py`, `hand-post-stats.txt` i PNG obok metadanych służą do podglądu
  ostatniego zrzutu. Skrypt obsługuje wybrane formaty; sprawdzać format i tile mode.
- Handheld: wejściowy RT 1408x786 i wyjście 1920x1080. Wcześniejszy wadliwy docked:
  tekstura 2095x1179, RT 2096x1179 i wyjście 2880x1620. Szerokość RT może być
  wyrównana, więc sama różnica jednego piksela nie dowodzi błędu.
- W ostatnim zrzucie `1-false-18/19` zapisują RT 0x810fe62000; `20` i `21` mają
  format A8B8G8R8_UNORM; `22` zapisuje wejście upscalera. Ostatni ma VS/PS
  0x2116575100 / 0x2116575400. Adresy shaderów mogą zmieniać się między sesjami.

Wszystkie wymienione artefakty są w `build/purple-investigation/` i pozostają lokalne.

### Wydajność — zbadana częściowo, bez naprawy

- Użytkownik zgłasza duże spadki przy patrzeniu w otwarty świat, mniejsze przy
  ścianie. Obserwowano około 11–16 FPS w badanym widoku, czasem 30–50 przy ścianie.
  Nie jest to stabilny benchmark ani wynik gotowej optymalizacji.
- Sampler GUI podpięto przez `main.cpp` i `src/yuzu/CMakeLists.txt` do istniejącego
  `yuzu_cmd/nx2_sampler.cpp`; uruchamia się wyłącznie po ustawieniu env.
- Profile: `world-samples.txt`, `world-post-samples.txt`, `world-post2-samples.txt`
  oraz `.folded`. VulkanWorker był głównie bezczynny; wątek GPU dużo czasu spędzał
  w obsłudze draw i oczekiwaniu na fence. Ostatni start miał faktycznie GPU High
  (`accurate=true` w logu), bez GPUBack. Nadal brakuje czystego profilu Low.
- Jeden pomiar obejmuje zrzuty, więc zawiera ich koszt. Tymczasowy `getenv` per
  draw też kosztuje. Nie polegać wyłącznie na nazwach symboli: linker ICF scala
  funkcje i symbolizacja może je mylić.
- `EDEN_NX2_GPU_PROFILE` nie był włączony. Nie włączać go do pomiaru bazowego;
  częste odczyty zegara w tym profilerze same istotnie obniżają wydajność.

### Pierwsze czynności przy wznowieniu

1. Wykonać analogiczny zrzut docked przy tej samej kamerze i porównać z
   `draw-probe.txt-captures/1-false-*`. Ustalić pierwszy etap, na którym znika scena:
   geometria/RT, aliasowanie tekstur czy późniejszy efekt. Nie obwiniać samego HMMA,
   skoro we wcześniejszym zrzucie jego tekstura wejściowa już była czarna.
2. Uruchomić z `EDEN_NX2_DRAW_PROBE` wskazującym pełną ścieżkę `draw-probe.txt`.
   128 = pasywny. Po wczytaniu wpisać 65664 (128+65536), by zrzucić jedną klatkę,
   potem przywrócić 128. Kolejny impuls wymaga wcześniejszego zgaszenia bitu 65536.
   Zrzut synchroniczny może chwilowo mocno zatrzymać grę.
3. Zachować zapis i profil A166B1F9...; nie tworzyć nowej gry. A ponownie jest na
   padzie, C nie jest już przyciskiem A. Nie nadpisywać całego INI starym backupem.
4. Po naprawie renderowania zmierzyć Low bez zrzutów i bez GPU_PROFILE. Samo
   obniżenie ustawień nie zostało potwierdzone jako rozwiązanie czarnego docked.
5. Przed uznaniem pracy za gotową usunąć tymczasowe probe/capture z
   `vk_rasterizer.cpp`, gettery diagnostyczne pipeline'ów i IR/SPV dump albo
   wydzielić je jako jasne opt-in bez kosztu normalnego draw. Przetestować oba
   tryby, zmiany rozdzielczości oraz dłuższe działanie zapisu.
6. Osobno sprawdzić zapętlone wideo tutoriala i log `SetTerminateResult`.

### Kod i odtworzenie sprawdzeń

- HMMA: `emit_spirv_tensor.cpp`, `spirv_emit_context.*`, `profile.h`,
  `lower_to_maxwell.h`, `vulkan_device.h`, `vk_pipeline_cache.cpp`.
- Ogólna zdolność tensorowa zamiast wyłącznie NV: `shader_cache.*`,
  `shader_environment.*`. Wersja cache Vulkan podniesiona 25→26.
- Testy: `src/tests/video_core/cooperative_matrix.cpp`; rzeczywisty test GPU:
  `tools/nx2/tensor_gpu_probe.cpp` (Vulkan SDK, MSVC x64 z AVX2/F16C).
- Tymczasowa diagnostyka: `vk_rasterizer.cpp`, `vk_compute_pipeline.h`,
  `vk_graphics_pipeline.h`; sampler: `src/yuzu/main.cpp`, `src/yuzu/CMakeLists.txt`.
- PowerShell: `. ./tools/windows/load-msvc-env.ps1`, `ninja -C build yuzu`.
  Testy włączono lokalnie: `cmake -S . -B build -DYUZU_TESTS=ON`;
  `ninja -C build tests`; `./build/bin/tests.exe '[tensor],[nx2_video]'`.
- Ustawić `EDEN_TENSOR_TEST_OUTPUT` na katalog i uruchomić `[tensor]`, potem
  `tensor_gpu_probe.exe <katalog>`. Probe sprawdza pojedynczy HMMA, łańcuch dwóch
  oraz warunkowe wykonanie jednej podgrupy przy drugiej nieaktywnej.
- Dowody: `focused-regression.log`, `gpu-arithmetic.log`, `tensor-tests*.log`.
  Ostatnie dodatki diagnostyczne przeszły build GUI i `git diff --check`; po nich
  nie powtarzano całego zestawu testów. Testy jednostkowe nie dowodzą dobrego docked.
- Uruchomienie: `build/bin/eglantine.exe -g "C:/Users/Marek/Desktop/EGLANTINE/GAMES/DKB ROM/updates/458752/exefs/main"`.
  Próby miały procesowe `EDEN_NX2_VDEC_EOS_WAIT=1`; nie zapisano tego globalnie.
  Nie utrwalono no-reuse, no-MME ani flag probe w ustawieniach użytkownika.
- ROM, zapisy gry, logi, build i zrzuty są ignorowane przez Git i zostają lokalne.
  `codex-wip.patch` i `git-status-final.txt` są dodatkową lokalną kopią kodu sprzed
  końcowego uzupełnienia MD; pełny stan kodu i dokumentacji obejmuje końcowy commit.
