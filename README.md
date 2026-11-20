# andropkg

Запуск Android-приложений (APK) на iPhone **без виртуальной машины и без JIT**. Это
«Wine для Android»: от Android остаётся только то, без чего приложение не работает
(ART + libcore, framework, публичные NDK-библиотеки). Ядро, init, zygote, system_server,
SurfaceFlinger, binder и HAL заменяются мостами к iOS.

В отличие от Andarium (полный LineageOS поверх User-mode Linux), каждый APK становится
обычным подписанным iOS-приложением. Полный план: [docs/PLAN.md](docs/PLAN.md).

## Как работает нативный код (M0)

Android и iOS — оба arm64, поэтому машинный код из `.so` исполняется без трансляции:

1. **Конвертер** (`converter/`, Swift) переупаковывает ELF в Mach-O dylib. Сегменты PT_LOAD
   лежат по тем же относительным адресам (ADRP/GOT продолжают работать), dylib
   подписывается обычным `codesign`, и dyld просто отображает его в память.
2. До подписи конвертер переписывает инструкции, которые на Darwin не работают, в переходы
   на трамплины:
   - `svc #0` → `andropkg_svc_entry` (`shim/`): эмуляция syscall Linux с сохранением
     всех регистров, как у ядра.
   - `mrs Xt, tpidr_el0` → загрузка bionic TP из слота Darwin TSD (через `tpidrro_el0`).
     Портится только Xt.
3. **Загрузчик** (`loader/`, C) применяет ELF-релокации (RELA, APS2, RELR), разрешает символы
   через шимы и вызывает init_array.

## Результаты M0 (iPhone 16 Pro, iOS 27.3, без JIT)

| Спайк | Результат |
|---|---|
| Код ELF `.so` из Android исполняется после упаковки в подписанный Mach-O | ✅ |
| Релокации RELATIVE / ABS64 / GLOB_DAT / JUMP_SLOT; RELA, APS2 (`android+relr`, дефолт clang для API ≥ 28), RELR | ✅ |
| init_array | ✅ |
| Патч `svc #0`: getpid/write/ENOSYS; x9–x17, x19, d0/d7/d16 и NZCV сохраняются | ✅ ~22 нс/вызов |
| `TPIDR_EL0` как bionic TLS | ❌ XNU хранит там своё значение (iOS 0x1004, macOS — номер CPU) и перезаписывает при переключении контекста |
| ↳ Обход: перезапись `mrs tpidr_el0` → слот TSD; 8 потоков, `-fstack-protector-strong` (`[tp+0x28]`) | ✅ +0.5–1.2 нс на чтение |
| x18 | XNU обнуляет; NDK резервирует x18 (`-ffixed-x18`), так что это безопасно |
| SIGSEGV-обработчик с подменой PC (неявные null-check в ART) | ✅ |
| `dlopen` упакованного образа | ~95 мс на iOS (≈470 мс на macOS с ad-hoc подписью) — **кешировать/предзагружать** |

## Результаты M1: ART + libcore Android 14 на iPhone (без JIT, без VM)

`scripts/runner-ios.sh` собирает рантайм под iOS, встраивает его в приложение и запускает
`tests/hello/Hello.java` на iPhone 16 Pro (iOS 27.3). VM стартует за 106 мс (отладочная сборка,
без boot image), `main()` выполняется за 21 мс: строки, коллекции, потоки, `wait`/`notify`,
GC (concurrent copying с read barrier), ICU, tzdata.

Как это устроено:
- **Сборка без Soong.** `tools/bp.py` и `tools/gen_cmake.py` строят CMake из `Android.bp`
  (`runtime/modules.py` задаёт модули и отклонения от AOSP). Клонируются только нужные проекты
  (`scripts/fetch-aosp.sh`, около 450 МБ, тег `android-14.0.0_r18`). Правки AOSP лежат в
  `runtime/patches/` и применяются автоматически.
- **Java-часть** берётся из официального образа эмулятора (`prebuilts/extract-android14.sh`).
- **Сжатые ссылки относительно базы.** На iOS нет памяти ниже 4 ГБ, а ART хранит ссылки как
  32-битные числа. Поэтому резервируется арена 4 ГБ, ссылка = `ptr − base`
  (`runtime/compat/include/andropkg/ref_arena.h`).
- **Ассемблер ART (ELF/GNU) переводится в Mach-O** при сборке (`tools/elf2macho_asm.py`,
  имена символов через `nm` — `tools/asm_symbols.py`). nterp пока отключён, работает
  C++-интерпретатор.
- **Слой совместимости с Linux** (`runtime/compat`): futex через `os_sync_wait_on_address`,
  epoll через kqueue, `pipe2`/`accept4`/xattr/константы, страница 16 КБ, `SIGIO` для
  прерывания блокирующего ввода-вывода.
- **Найдены скрытые ошибки AOSP**, которые в Android маскируются окружением:
  - `BitVector` поверх неинициализированного `alloca` (в AOSP спасает `-ftrivial-auto-var-init=zero`);
  - `sizeof(std::array<T,0>)` при `data() == nullptr` в новой libc++.

Ограничения:
- **BoringSSL ещё не собран:** у `BigInteger` нет ускоренных путей.
- **JNI-функции с больше чем 8 аргументами** не учтены: на Darwin стековые аргументы
  раскладываются иначе.
- **Только отладочная сборка и интерпретатор.**

## Запуск

```sh
brew install lld xcodegen              # тестовые ELF собираются без NDK (freestanding)
cp Config/Local.xcconfig.example Config/Local.xcconfig   # DEVELOPMENT_TEAM, префикс bundle id

scripts/spikes-mac.sh                  # быстрый цикл на Apple silicon Mac
scripts/spikes-ios.sh                  # спайки M0 на iPhone
scripts/fetch-aosp.sh && prebuilts/extract-android14.sh   # исходники AOSP + Java-часть Android 14
scripts/run-dalvikvm-mac.sh -cp build/tests/hello/classes.dex Hello   # ART на Mac (после cmake/ninja)
scripts/runner-ios.sh                  # ART на iPhone: сборка, установка, запуск Hello
(cd converter && swift test)           # юнит-тесты конвертера

converter/.build/debug/andropkg wrap-elf libfoo.so -o libfoo.elf.dylib --platform ios
```

## Структура

```
converter/   Swift: разбор ELF, упаковка ELF→Mach-O, перезапись инструкций (CLI + библиотека для приложения-контейнера)
loader/      C: ELF-линкер для упакованных образов
shim/        C/asm: эмуляция syscall Linux, bionic TLS (будущий шим libc bionic→Darwin)
spikes/      M0: тестовая .so (testlib), общие проверки, harness для Mac, iOS-приложение
docs/        план
```

## Дальше

- **M2:** framework.jar в одном процессе с лёгкими системными сервисами, Activity с отрисовкой
  через hwui → MoltenVK.
- **TODO в загрузчике до M4:**
  - TLS-релокации (TLSDESC / TPREL) и статические TLS-блоки в TCB;
  - DT_NEEDED между образами, RELRO → read-only, `dl_iterate_phdr` для раскрутки стека в C++;
  - ленивая привязка TCB для потоков, созданных не через шим.
- **ELF с выравниванием 4 КБ** конвертер пока отвергает (сегменты делят страницу 16 КБ).
