# Orbit Native

Экспериментальный перенос [Orbit](https://github.com/TrashDokiDoki/Orbit) на C++17 и Qt Widgets для Windows и Linux. Нативная сборка использует виджеты Qt и не запускает Electron, Node.js, браузерный движок или PowerShell. История исходного проекта сохранена в репозитории. Старый интерфейс находится в `resources/app`; нативная цель использует оттуда только изображения.

## Что доступно в первом прототипе

- Свёрнутый островок с часами, погодой и мопсом. Панель раскрывается по кнопке или наведению.
- Лаунчер Spotify, ChatGPT и Codex, полка файлов с перетаскиванием, заметка и таймер.
- На Linux плеер получает название, исполнителя и локальную обложку через [MPRIS](https://specifications.freedesktop.org/mpris/latest/Player_Interface.html). На Windows работают системные медиаклавиши. Название и обложка на Windows ещё не перенесены.
- На Wayland положение окна и режим поверх остальных зависят от композитора. Для точного расположения сверху используйте X11 или правило окна вашего композитора.
- Полный прогноз, синхронные тексты, захват экрана, OCR, история буфера, VPN и вкладка сетевых инструментов остаются в исходном Orbit. Эта версия пока не заменяет его целиком.

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

Первые сборки рассчитаны на x64. Для ARM64 нужны отдельные Qt SDK и проверка на устройстве. Настройки Orbit Native сохраняются отдельно. Погода использует Open-Meteo, лаунчер открывает ссылки в браузере по нажатию.

[Лицензия Orbit](resources/app/LICENSE)
