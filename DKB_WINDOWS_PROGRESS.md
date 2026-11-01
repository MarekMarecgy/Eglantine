# Donkey Kong Bananza — gałąź `windows-dkb-fixes`, stan i postępy

Gra: Donkey Kong Bananza (`040074A01BF12000`), update v0.6.0.
Gałąź bazuje na `nx2` i zawiera tylko te poprawki z `macos-moltenvk-dkb-fixes`,
które nie zależą od platformy. Szczegółowy dziennik śledztwa (macOS) jest na gałęzi macOS.

## Co jest na tej gałęzi (2026-11-01)

### 1. Abort 2111-3604 przy zapętlonym filmiku podpowiedzi — naprawione

Commit `50b4ee7bb5` (cherry-pick `caa117b0f2`), plik `src/core/hle/service/nx2_gsv.h`.

- Objaw: po pewnym czasie gry zawieszenie (0 kl/s); w logu `SetTerminateResult 0x1c286f (2111-3604)`
  i `Userspace PANIC`.
- Przyczyna: filmik podpowiedzi (VP9 960×540, 180 klatek) jest zapętlany. Po klatce końca strumienia
  gra oddaje obrazy, robi `flush`, potem `Start` (6) z uchwytem na syncpoint dekodera i czyta
  po jednym wyjściu (`GetOutputInfo`, 213) na każdy oddany obraz. `Start` był ignorowany, więc
  odczyty nic nie znajdowały i gra wykonywała abort.
- Poprawka:
  - `Start` podbija syncpoint do podanej wartości i zamienia oddane do niej obrazy na gotowe wyjścia.
  - Obrazy oddane po końcu strumienia trafiają na osobną listę i dostają klatkę końca strumienia
    zamiast błędu.
- Wynik (macOS): filmik zapętla się wielokrotnie bez `PANIC`. Logika jest wspólna dla wszystkich platform.

### 2. Ostrzeżenie o atrybutach wierzchołków poza limitami urządzenia

Commit `e5c0bb5535` (cherry-pick `cf076563b9`), plik
`src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp`.

- Tylko diagnostyka: `LOG_WARNING`, gdy atrybut wskazuje binding lub location poza limitami urządzenia.
  Na Windowsie (pełny Vulkan) nie powinno się pojawiać.

## Czego celowo nie przeniesiono (tylko macOS / MoltenVK)

| Zmiana na gałęzi macOS | Powód |
| --- | --- |
| Własna `CAMetalLayer` dla MoltenVK (`qt_common`) | Zator `QMetalLayer` w Qt 6.11 dotyczy tylko macOS |
| `Unsafe_IgnoreGlobalMonitor` w backendzie arm64 Dynarmic | Backend x64 już omija globalny monitor przy dokładności „Auto” |
| Emulacja shadera geometrii terenu DKB | MoltenVK nie ma geometry shaderów; GPU na Windowsie je mają |
| Konwersja indeksów Uint8 → Uint16 na CPU | Koszt konwersji występuje tylko w MoltenVK |
| Wspólny sampler ponad limit Metal na etap | Limit samplerów Metal |

Jeśli na Windowsie brakuje błota/tunelu albo widać błędy `Geometry streams is not implemented`,
częściowo przydatna może być zmiana w `emit_spirv_special.cpp` (emisja tylko strumienia 0
zamiast wyjątku) — do sprawdzenia.

## Otwarte problemy

- **Abort 2162-0001** ok. 0,5 s po autozapisie po zebraniu pierwszego banana. Wystąpił raz,
  przy kolejnym uruchomieniu (>4 min gry) się nie powtórzył — możliwy wyścig zależny od czasu.
  Gdy wróci: sprawdzić ostatnie wpisy `ipc`/`tipc` przed `SetTerminateResult` (`EDEN_NX2_IPC_TRACE=1`).
- **Czarna sylwetka DK** w przerywniku „Oh, Banana!” — nie badane; w logu brak pominiętych
  pipeline'ów i odrzuconych shaderów.
- **Wydajność na Windowsie** — niezmierzona. Pomiary z macOS (~12–15 kl/s, ograniczenie GPU)
  wynikają głównie z MoltenVK i nie przenoszą się na Windows.

## Do zrobienia na Windowsie

1. Zbudować gałąź i uruchomić DKB; potwierdzić, że filmik podpowiedzi zapętla się bez abortu 2111-3604.
2. Zmierzyć kl/s w poziomie (`EDEN_NX2_GPU_PROFILE=1`, `EDEN_NX2_IPC_SUMMARY=1`; `vi2::229` = klatki).
3. Sprawdzić, czy teren (błoto, tunel) renderuje się poprawnie z natywnym geometry shaderem.
4. Obserwować abort 2162-0001 po pierwszym autozapisie.
