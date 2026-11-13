# Orbit Native

![Orbit Native 0.2.0: островок, главная, погода, полка, заметки, таймер, напоминания, буфер и инструменты](docs/orbit-native-demo.gif)

Экспериментальный перенос [Orbit](https://github.com/TrashDokiDoki/Orbit) на C++17 и Qt Widgets для Windows и Linux. Нативная сборка использует виджеты Qt и не запускает Electron, Node.js, браузерный движок или PowerShell. История исходного проекта сохранена в репозитории. Старый интерфейс находится в `resources/app`; нативная цель использует оттуда только изображения.

## Установка

В [Releases](https://github.com/TrashDokiDoki/Orbit-Native/releases) публикуются два пакета x64. На Windows запустите `OrbitNative-0.2.0-windows-x64.exe`. На Ubuntu 24.04 установите `orbit-native_0.2.0_amd64.deb` командой `sudo apt install ./orbit-native_0.2.0_amd64.deb`. Откройте Orbit Native из меню приложений. Совместимость с другими дистрибутивами зависит от версий Qt и системных библиотек.

## Что доступно

- Свёрнутый островок с часами, погодой и мопсом. Панель раскрывается по кнопке или наведению.
- Лаунчер Spotify, ChatGPT и Codex, полка файлов с перетаскиванием, заметка, таймер и разовые или ежедневные напоминания.
- Почасовой и недельный прогноз Open-Meteo, преобразование текста, история текстового буфера по согласию пользователя и сохранение снимка монитора на Windows и X11.
- На Linux плеер получает название, исполнителя и локальную обложку через [MPRIS](https://specifications.freedesktop.org/mpris/latest/Player_Interface.html). На Windows работают системные медиаклавиши. Название и обложка на Windows ещё не перенесены.
- На Wayland положение окна и режим поверх остальных зависят от композитора. Для точного расположения сверху используйте X11 или правило окна вашего композитора.
- Синхронные тексты, запись экрана, OCR, QR, управление системным звуком, VPN и вкладка сетевых инструментов пока остаются в исходном Orbit. Снимок экрана под Wayland потребует отдельного адаптера портала.

В ветке разработки добавлены несколько заметок с переносом старой записи, пауза таймера и восстановление таймера после перезапуска. GIF показывает опубликованный Preview 0.2.0.

## Сборка

Нужны компилятор C++17, CMake 3.21+ и Qt 6.4+ с Widgets и Network. На Linux нужен модуль Qt DBus.

На Ubuntu 24.04:

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-qpa-plugins
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/OrbitNative
```

На Windows откройте терминал Visual Studio с C++ и установите Qt для MSVC. Укажите путь к установленному комплекту Qt через `CMAKE_PREFIX_PATH`:

```powershell
cmake -S . -B build -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
windeployqt --release build/Release/OrbitNative.exe
build/Release/OrbitNative.exe
```

Для создания пакета запустите `cpack --config build/CPackConfig.cmake -B package` на Linux или добавьте `-C Release` на Windows с установленным NSIS. Первые пакеты рассчитаны на x64. Для ARM64 нужны отдельные Qt SDK и проверка на устройстве. Настройки Orbit Native сохраняются отдельно. Погода использует Open-Meteo, лаунчер открывает ссылки в браузере по нажатию.

[Лицензия Orbit](resources/app/LICENSE)

## Перенос интерфейса Orbit 0.3.3

Экспериментальная цель `OrbitShell` загружает исходные HTML, CSS, JavaScript и изображения Orbit. На Windows она использует системный WebView2, на Linux — GTK 3 и WebKitGTK 4.1. Qt обслуживает данные и системные запросы. WinRT используется в медиамодуле Windows, а Linux получает медиасессии через MPRIS.

```bash
cmake -S . -B build -DORBIT_WEB_SHELL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OrbitShell --parallel
```

Для сборки оболочки на Ubuntu нужны `libgtk-3-dev`, `libwebkit2gtk-4.1-dev`, `libgtk-layer-shell-dev` и `libpulse-dev`, а для интеграции X11 нужен `libxtst-dev`. На Windows нужен WebView2 Runtime и компилятор с C++20. CMake загружает закреплённую версию библиотеки webview 0.12.0.

- Плеер получает трек, состояние, обложку и позицию от медиаприложения. Синхронизированные тексты загружаются из LRCLIB. Обложка и данные зависят от того, что передаёт выбранный источник.
- Настройки открываются в отдельном окне. Подключены масштаб и положение островка, компактный плеер, лаунчер, полка, история текста и изображений, заметки, таймер и напоминания.
- Звук использует Core Audio на Windows и PulseAudio или PipeWire-Pulse на Linux. Редактор эквалайзера сохраняет профиль Equalizer APO.
- Снимок открывает редактор с разметкой, пипеткой, QR и OCR. На Wayland для снимков используется системный портал. Запись экрана сохраняется в MP4 или WebM в зависимости от движка.
- Панель управления использует системные настройки, службы питания и Happ. Вкладка «Сеть» переключает установленную службу Zapret и выбранный TG WS Proxy.

На Linux пакеты CI включают Vosk и модели для русского и английского голосового ввода. В CI распознавание проверяется на аудиозаписи. Захват и позиционирование на Wayland зависят от порталов и протоколов композитора. Проверка на реальных рабочих столах продолжается.

### Проверенные сборки

- В Windows 10 22H2 x64 проверены установка через графический мастер, первый запуск и удаление. На тестовой системе WebView2 уже был установлен.
- Единый установщик проверен в CI на Windows x64 и ARM64. Для Ubuntu 24.04 в CI проверены установка, запуск и удаление пакетов x64 и ARM64. [Прогон сборки](https://github.com/TrashDokiDoki/Orbit-Native/actions/runs/43362169190)
- На Ubuntu/X11 проверены отрисовка вкладок и вставка в отдельный редактор через `Ctrl+Alt+Shift+V`. На Wayland проверен запуск под Weston. Для вставки на Wayland используйте обычную команду вставки редактора. [Проверка X11](https://github.com/TrashDokiDoki/Orbit-Native/actions/runs/43362869859)

### Установщики WebView Preview

[Скачать Windows EXE и Ubuntu DEB](https://github.com/TrashDokiDoki/Orbit-Native/releases/tag/v0.3.0-shell-preview).

Сборки с `-DORBIT_WEB_SHELL=ON -DORBIT_PACKAGE_SHELL=ON` выпускают отдельный пакет предварительной версии. Windows x64 получает NSIS-установщик с библиотеками Qt. При отсутствии WebView2 установщик сам запускает подписанный загрузчик Microsoft. Для загрузки рантайма нужен интернет. Сборщику Windows нужно передать `-DORBIT_WEBVIEW2_BOOTSTRAPPER=<путь к подписанному EXE Microsoft>`.

На Ubuntu Desktop 24.04 x64 откройте скачанный `.deb` через «Центр приложений», нажмите «Установить» и подтвердите установку паролем, если система его запросит. Центр приложений получает зависимости из репозиториев Ubuntu. После установки запустите Orbit Native из меню приложений. [Документация Ubuntu](https://ubuntu.com/desktop/docs/en/24.04/explanation/snap-and-deb-packages/#deb-packages)

WebView Preview добавлен в меню приложений отдельно от Qt Widgets. В нём работает проверка стабильных релизов GitHub, значок в трее при наличии системного трея и экспорт данных из настроек.

### Сборки в GitHub Actions

В [workflow оболочки](https://github.com/TrashDokiDoki/Orbit-Native/actions/workflows/shell.yml) можно скачать установщик `OrbitNative-<версия>-Windows.exe` из артефакта `OrbitShell-windows-installer-universal`. Он выбирает x64 или ARM64, устанавливает приложение для текущего пользователя и включает Tesseract с русским и английским языками. Если в системе отсутствует WebView2, для его автоматической установки потребуется интернет.

Для Ubuntu 24.04 доступны `OrbitShell-linux-installer-x64` и `OrbitShell-linux-installer-arm64`. Распакуйте архив артефакта и откройте файл `.deb` в менеджере пакетов. Он установит зависимости из репозиториев Ubuntu. В CI проверяются установка, запуск и удаление пакета на обеих архитектурах. Поддержка других дистрибутивов этой проверкой не покрывается.
