# andropkg — Android-приложения на iOS без VM (облегчённый «Wine для Android»)

## Контекст

Andarium запускает на iPhone полный LineageOS (Android 17) поверх ядра User-mode Linux: это тяжёлая VM, которая работает только на переднем плане и запускает целую ОС ради одного приложения. Нам нужно другое: запускать отдельные APK как обычные iOS-приложения, оставив от Android только то, без чего приложение не работает.

Минимальный набор выглядит так:
- **ART + libcore** выполняют dex-байткод.
- **framework.jar + framework-res** дают API `android.*`.
- **Нативные библиотеки framework**: hwui/Skia, androidfw (ресурсы), minikin/harfbuzz/freetype (текст), ICU.
- **Публичные NDK-библиотеки**: libc, libm, libdl, liblog, libandroid, EGL/GLES, Vulkan, OpenSLES/AAudio, jnigraphics, z. Начиная с API 24 Android сам запрещает приложениям линковаться с чем-то кроме этого списка, поэтому он и задаёт нативный минимум.

Всё остальное заменяется мостами к iOS или удаляется: ядро, init, zygote, system_server, SurfaceFlinger, драйвер binder, HAL'ы и SELinux.

Решения пользователя:
- **Доставка:** гибрид. Есть конвертер APK→IPA на Mac и приложение-контейнер, которое ставит APK прямо на iPhone. Оба используют один рантайм.
- **MVP:** сразу с поддержкой NDK.
- **Сборка:** есть только Mac, Linux-шаги выполняем в Docker.
- **Версия Android:** 14 (API 34).

## Ключевые архитектурные решения

1. **JIT не нужен.** На iOS 26 JIT для сайдлоада ненадёжен: TXM, поломки в 26.4. Поэтому весь исполняемый код должен быть подписан заранее.
   - Рантайм (ART, hwui и остальное) собирается под iOS как обычный подписанный код: это не трансляция, а нативная сборка через Xcode toolchain.
   - Байткод выполняет интерпретатор ART (nterp или switch). На этапе M6 к нему добавится AOT: вывод dex2oat упаковывается и подписывается так же, как `.so`.
2. **Нативные `.so` из APK: ELF внутри подписанного Mach-O.** Android и iOS оба arm64, поэтому машинный код переносится без изменений.
   - Конвертер собирает Mach-O dylib, в котором сегменты PT_LOAD лежат по тем же относительным адресам. Это важно, чтобы не сломались `adrp` и обращения через GOT. Затем dylib подписывается.
   - Наш ELF-линкер (на основе bionic linker) получает образ через `dlopen`. Он применяет релокации к страницам данных, разрешает символы через шимы и вызывает `init_array`.
   - Инструкции `svc #0` патчатся ещё до подписи: они заменяются переходами на трамплины в libc-шиме.
   - Основная цель — ELF с выравниванием 16 КБ. Google Play требует его с ноября 2025 года, и оно совпадает с размером страницы iOS. Для ELF с выравниванием 4 КБ нужен отдельный путь (см. «Риски»).
3. **Шим libc (bionic ABI → Darwin).** Идея взята из bionic_translation проекта ATL:
   - трансляция структур (`stat`, `dirent`, `sigaction`) и кодов errno;
   - `epoll` через kqueue, `futex` через `os_sync_wait_on_address`/`__ulock`, `eventfd`;
   - эмуляция `/proc` и `/sys`, `prctl` и `ioctl`;
   - TLS для bionic через `TPIDR_EL0`;
   - трансляция путей: `/data/data/<pkg>` и `/sdcard` отображаются в песочницу iOS.
4. **Настоящий framework AOSP 14 вместо переписанного, как в ATL.** Так выше совместимость. Работает один процесс без zygote.
   - `ServiceManager` заполняется локальными Java-реализациями AIDL-интерфаceов («mini system_server»): IActivityManager, IPackageManager, IWindowSession/IWindowManager, IInputMethodManager, IDisplayManager, IPowerManager, IAlarmManager, IConnectivityManager, INotificationManager, IClipboard, IAudioService, ILocationManager.
   - `Binder.queryLocalInterface` возвращает локальный объект, поэтому драйвер binder не нужен.
   - Патчи framework.jar не нужны: подмена делается через `ServiceManager.initServiceCache` и наш jar в начале boot classpath.
5. **Графика.**
   - hwui работает через свой `SkiaVulkanPipeline` поверх MoltenVK, а `ANativeWindow` привязывается к `CAMetalLayer`.
   - Каждое окно Android становится отдельным UIView. Композицию выполняет iOS, так что SurfaceFlinger не нужен.
   - GLES приложений идёт через ANGLE (бэкенд Metal), Vulkan приложений — через MoltenVK.
6. **Остальные мосты.**
   - Касания: UITouch превращается в MotionEvent и идёт через InputChannel (socketpair на iOS работает).
   - Клавиатура: IME связывается с UITextInput.
   - Звук: AudioTrack, AAudio и OpenSL ES идут через AudioUnit.
   - Жизненный цикл: состояния UIApplication отображаются на жизненный цикл Activity.
   - Позже: MediaCodec через VideoToolbox, WebView через WKWebView, microG вместо GMS.
7. **Гибридная доставка.**
   - Конвертер написан на Swift. Он разбирает AndroidManifest, генерирует Info.plist и иконки, упаковывает `.so` и dex и подписывает результат.
   - Этот же Swift-код встроен в приложение-контейнер. Там он подписывает dylib сертификатом сайдлоада на устройстве (как LiveContainer через zsign), поэтому APK можно ставить без компьютера.

## Решения по итогам спайков (проверено на iPhone 16 Pro, iOS 27.3)

- **TPIDR_EL0 занят XNU.** Ядро хранит там своё значение и перезаписывает его при переключении контекста. Поэтому конвертер переписывает каждое чтение `mrs Xt, tpidr_el0` в загрузку указателя bionic TCB из слота Darwin TSD через `tpidrro_el0` (M0).
- **Памяти ниже 4 ГБ на iOS нет**, а `-pagezero_size` ядро запрещает: процесс убивается при запуске. ART хранит ссылки как 32-битные числа (куча ниже 4 ГБ), поэтому в ART вводятся **сжатые ссылки относительно базы**:
  - при старте резервируется одна арена (iOS даёт непрерывные 4 ГБ, 8 ГБ уже нет);
  - `ref = ptr − base`, `null = 0` (`PtrCompression` в `object_reference.h`);
  - все выделения `MemMap` с `low_4gb` нарезаются из этой арены;
  - адрес пересылки в `LockWord` хранится относительно базы;
  - `ObjPtr`-poisoning отключается.
- **Java-часть Android 14** берётся из официального образа эмулятора `arm64-v8a-34_r04` (`prebuilts/extract-android14.sh`): 35 boot-jar'ов, framework-res, ICU 72, tzdata, шрифты. Исходники C++ взяты из тега `android-14.0.0_r18`: набор native-методов `VMRuntime` совпадает с образом во всех тегах 14.0.0.
- **Сборка без Soong.** `tools/bp.py` разбирает `Android.bp` (раскрывает `defaults`, варианты `arch`/`target`, `filegroup`) и генерирует CMake. Клонируются только нужные проекты AOSP (`scripts/fetch-aosp.sh`, около 430 МБ).

## Структура репозитория (с нуля, каталог пуст)

```
andropkg/
  third_party/   # сабмодули: AOSP 14 (art, libcore, frameworks/base subset, libnativehelper,
                 #   bionic/linker), skia, ANGLE, MoltenVK, icu, harfbuzz, freetype, zsign; art_standalone как референс
  prebuilts/     # скрипт извлечения Java-части Android 14 из образа LineageOS 21 (Docker: simg2img/erofs-utils):
                 #   framework.jar, core-oj/core-libart/okhttp/bouncycastle (APEX ART), framework-res.apk, шрифты, icudt
  runtime/       # CMake → iOS xcframework: порт ART (Darwin, без JIT), libandroid_runtime (урезанный), hwui, androidfw, minikin
  shim/          # libc-шим (bionic ABI → Darwin), /proc, futex, epoll, TLS, трансляция путей
  loader/        # ELF-линкер для упакованных образов, резолвер символов NDK
  ndk-libs/      # реализации публичных NDK-библиотек: libandroid, EGL→ANGLE, vulkan→MoltenVK, OpenSLES/AAudio→AudioUnit
  services/      # Java: mini system_server (локальные AIDL-сервисы), сборка в Docker через javac + d8
  bridges/       # Swift/ObjC++: окна/Metal, ввод, IME, жизненный цикл, аудио
  converter/     # Swift package: APK→IPA (manifest, ресурсы, упаковка ELF в Mach-O, патч svc, подпись); CLI + библиотека
  host-app/      # Xcode: AndroPkg Container (установка APK на устройстве, подпись, лаунчер)
  tools/         # генератор JNI-заглушек: сканирует native-методы framework и создаёт логирующие заглушки
  tests/         # корпус тестовых APK + XCTest
```

## Этапы

- **M0. Спайки, которые решают, жизнеспособен ли проект.** Проверяются на реальном iPhone.
  - (a) Сохраняется ли `TPIDR_EL0` при переключении потоков iOS и можно ли разместить там TLS для bionic.
  - (b) Загружается ли упакованный ELF→Mach-O через `dlopen`, выполняется ли код NDK-`.so` (простая функция и вызов через GOT) и работает ли патч `svc`.
  - (c) Остаётся ли нетронутым x18.
  - (d) Работает ли обработчик SIGSEGV, который нужен ART для неявных null-check.
- **M1. ART + libcore на iOS.** В режиме только интерпретатора консольный dex печатает «Hello» в os_log. Шим используется только для самого рантайма.
- **M2. Framework в одном процессе.** Загружается boot classpath, запускаются локальные сервисы, стартует `ActivityThread`. `Activity` с `TextView` отрисовывается через hwui → MoltenVK → CAMetalLayer.
- **M3. Интерактивность.** Касания, IME, ресурсы и конфигурация (плотность экрана, тёмная тема), жизненный цикл, SharedPreferences, SQLite, сеть. Цель — открытое Java-приложение из F-Droid.
- **M4. NDK (цель MVP).** Загрузчик `.so`, полный шим libc, libandroid (ANativeActivity, AAssetManager, ALooper), EGL/GLES через ANGLE, звук. Цель — простая NDK-игра на GLES из открытых исходников.
- **M5. Доставка.**
  - CLI-конвертер `andropkg convert app.apk -o app.ipa`.
  - Приложение-контейнер с установкой APK и подписью на устройстве.
- **M6. Производительность и охват.**
  - AOT-boot-image framework и dex2oat для приложений, упакованные и подписанные.
  - MediaCodec, WebView-мост, уведомления, microG.

## Риски и открытые вопросы

- **ELF с выравниванием 4 КБ.** Код и данные могут оказаться на одной странице 16 КБ. Нужна перекомпоновка с сохранением расстояний для `adrp`; если это невозможно, такие библиотеки не поддерживаются.
- **Приложения с защитой от вскрытия.** Они делают прямые syscalls из зашифрованного кода, и патч `svc` до подписи до него не дотянется. Такие приложения не поддерживаются.
- **libandroid_runtime огромна** (сотни JNI-файлов). Портируем её постепенно, а для остального используем автогенерируемые заглушки.
- **Лицензии.** ATL и bionic_translation распространяются под GPL-3. Если брать их код, а не только идеи, проект станет GPL-3. Решить до M1.
- **Распространение.** App Store не подходит, только сайдлоад: AltStore/SideStore, TrollStore, AltStore PAL в ЕС.

## Проверка

- **Спайки M0:** XCTest на устройстве с проверками (TLS между потоками, вызов функции из упакованного `.so`, перехват `svc`).
- **На каждом этапе:** сборка `xcodebuild` для реального устройства и запуск эталонного APK из `tests/`. Сверяем скриншот окна с эталоном и проверяем, что в логе нет фатальных ошибок JNI-заглушек.
- **Регрессии:** скрипт гоняет корпус APK через конвертер, ставит IPA на подключённое устройство (`xcrun devicectl device install app`), запускает их и собирает os_log и краши.
- **Юнит-тесты:** в `converter/` (разбор манифеста, упаковка ELF→Mach-O, патч `svc`) через `swift test` на Mac. Тесты шима гоняются на iOS Simulator с arm64.
