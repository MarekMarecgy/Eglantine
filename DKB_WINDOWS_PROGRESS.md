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
- Uwaga (2026-11-02): ta poprawka usuwa abort 2111-3604, ale zostawia abort 2162-0001 — patrz
  „Abort 2162-0001” w otwartych problemach.

### 2. Ostrzeżenie o atrybutach wierzchołków poza limitami urządzenia

Commit `e5c0bb5535` (cherry-pick `cf076563b9`), plik
`src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp`.

- Tylko diagnostyka: `LOG_WARNING`, gdy atrybut wskazuje binding lub location poza limitami urządzenia.
  Na Windowsie (pełny Vulkan) nie powinno się pojawiać.

### 3. Fioletowy ekran w poziomie (widać tylko HUD) — naprawione (2026-11-02)

Commit `a21d96beba` (cherry-pick `8dae6f3467` + domyślne włączenie), plik
`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`.

- Objaw: po wczytaniu poziomu (tunel kopalni) cały ekran ma kolor tła (fiolet), widać tylko HUD.
  Ten sam objaw był na początku na macOS. Testowane na RTX 5070 Ti, sterownik 617.14.
- Przyczyna: teren (błoto, tunel) jest rysowany w dwóch przebiegach — geometry shader odsiewa
  trójkąty i dopisuje ich indeksy do listy (licznik pod adresem z c0[0x410], lista z c0[0x420]),
  a drugi przebieg rysuje z tej listy. Na NVIDIA natywny geometry shader działa, ale jego zapisy
  nie docierają do drugiego przebiegu, więc lista jest pusta. Przyczyna po stronie emulatora
  (śledzenie zapisów GS do pamięci) nie jest znana.
- Poprawka: shader terenu DKB (hash `0x5b7bdd2ad167a039`) idzie ścieżką emulowaną z macOS na
  każdym urządzeniu (wszystkie trójkąty, bez odsiewania). `EDEN_NX2_TERRAIN_GS_EMULATION=0`
  przywraca natywny etap.
- Wynik: tunel, DK, skały i złoto rysują się poprawnie.

## Czego celowo nie przeniesiono (tylko macOS / MoltenVK)

| Zmiana na gałęzi macOS | Powód |
| --- | --- |
| Własna `CAMetalLayer` dla MoltenVK (`qt_common`) | Zator `QMetalLayer` w Qt 6.11 dotyczy tylko macOS |
| `Unsafe_IgnoreGlobalMonitor` w backendzie arm64 Dynarmic | Backend x64 już omija globalny monitor przy dokładności „Auto” |
| Konwersja indeksów Uint8 → Uint16 na CPU (`cebfbe2155`) | Koszt konwersji występuje tylko w MoltenVK |
| Wspólny sampler ponad limit Metal na etap | Limit samplerów Metal |

Emulacja shadera geometrii terenu jednak **jest** przeniesiona — patrz punkt 3.

## Otwarte problemy

### Abort 2162-0001 — przyczyna zawężona, poprawka niesprawdzona (2026-11-02)

Powtarzalny na Windowsie (3 sesje na 3, po 72–116 s gry), także na macOS „w losowych momentach”.
Kiedy pada tylko wątek filmiku, gra działa dalej, a w okienku samouczka zostaje żółty prostokąt
z bananem. Zawsze sprawdzać `SetTerminateResult` w logu, zanim uzna się test za udany.

- **Gdzie:** `SilverStar.nss +0xDFCDC8` w odtwarzaczu filmików samouczka (okienko w prawym dolnym
  rogu, VP9 960×540). Odtwarzacz przy zamykaniu filmu oddaje ramki do puli biblioteki wideo
  (`0xdfcd00`), a jednej ramki nie ma na liście „w użyciu” → `AbortImpl`.
- **Nie** jest to wyścig CPU: na CPU Accuracy = Accurate abort wystąpił tak samo.
- **Budowa biblioteki** (odczytana z disasemblacji, importy nazwane z tablicy relokacji):
  - pula ramek ma 8 węzłów (`0xdfca00`: `cmp x9,#8; csel` — slot ≥ 8 nadpisuje węzeł 0);
  - `Acquire*DecoderOutputBuffer` w SDK jest po stronie klienta (pierścień 8 obrazów w kolejności
    zwalniania, z fence'em z odpowiedzi na `Release`); pusty pierścień → 2111-3604;
  - `Get*DecoderOutputBufferProperties` pyta `GetOutputInfo` (213) w pętli, aż dostanie żądany obraz;
  - flush biblioteki (`0xdfc4c0`) zwalnia **wszystkie** ramki z kolejki i z listy „w użyciu”,
    także te, które odtwarzacz wciąż trzyma w swoich tablicach (`+0x1a8`, `+0x1b8`).
- **Ślad** (`EDEN_NX2_VDEC_TRACE=1`): stan IPC jest zbilansowany (np. 317 wyjść / 316 zwolnień),
  ale po każdym zapętleniu rytm zwalniania obrazów rośnie: 4,5 → 4,4,5 → 4,4,4,5,5.
- **Hipoteza:** po końcu filmu emulator od razu oddaje powtórki klatki końcowej dla zwalnianych
  obrazów (fence już osiągnięty). Odtwarzacz je pobiera, a flush biblioteki zaraz je zwalnia —
  w tablicach odtwarzacza zostają nieaktualne wpisy dla tych samych obiektów obrazów (4, 5),
  które później są zwalniane podwójnie. Na konsoli takie obrazy pewnie czekają do `Start`.
- **Sprawdzone i odrzucone:** brak powtórek (→ 2111-3604); powtórki bez flagi końca (→ dalej
  2162-0001); obrazy kończone przez `Start` z flagą końca (→ filmik restartuje się w kółko i pada
  z 2111-3604).
- **Poprawka do sprawdzenia:** `EDEN_NX2_VDEC_EOS_WAIT=1` — obraz zwolniony po końcu filmu dostaje
  przyszły fence i czeka na `Start` jak każdy inny. Domyślnie wyłączone (Mario Kart World
  potrzebował natychmiastowego fence'a po filmie tytułowym). Jedyny test: brak abortu, ale żaden
  filmik nie doszedł do końca i się nie zapętlił — **niezweryfikowane**. Test: stać przy samouczku
  ≥ 1 min (kilka pętli), potem sprawdzić w logu `SetTerminateResult` i wpisy `trace:`.
- Metoda: rozpakowanie `exefs/main` i `exefs/sdk` (NSO, LZ4), disasm `llvm-objdump` (z VS Build
  Tools) z nazwami importów z relokacji MOD0, wyszukiwanie wywołań `BL` do danych adresów.
  Skrypty nie są w repozytorium.

### Pozostałe

- **Klaskanie DK (Hand Slap) nie podświetla skarbów ukrytych w skale** — widać to też na filmiku
  samouczka. Nie badane.
- **Kwadratowe artefakty w filmikach samouczków** (pomarańczowe kwadraty na żółtym tle) —
  nie badane; możliwy problem dekodera VP9 albo kopiowania obrazu.
- **Upscaler tensorowy** (compute 2880×1620, HMMA) jest odrzucany: na kartach Ampere+ sterownik ma
  wyłączone float16, więc `EDEN_NX2_TENSOR=fast` się nie włącza, a zastępczy blit
  (`UpscaleInsteadOfNativeProgram`) szuka uchwytów pod cbuf0+0x90/0xa0, których DKB nie używa.
  Obraz i tak się wyświetla.
- **Czarna sylwetka DK** w przerywniku „Oh, Banana!” — nie badane; w logu brak pominiętych
  pipeline'ów i odrzuconych shaderów.
- **Wydajność:** 23–36 kl/s na Ryzen 7 9800X3D + RTX 5070 Ti, GPU ~25% — ograniczenie po stronie
  CPU/wątku GPU emulatora, nie karty.

## Do zrobienia na Windowsie

1. Zweryfikować `EDEN_NX2_VDEC_EOS_WAIT=1` przy kilku pętlach filmiku; jeśli działa, rozważyć
   włączenie domyślne tylko dla DKB albo sprawdzić z Mario Kart World.
2. Hand Slap nie podświetla skarbów.
3. Artefakty w filmikach.
4. Natywny geometry shader terenu: dlaczego zapisy nie docierają do drugiego przebiegu.
