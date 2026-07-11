# YMHub

Компактный мини-плеер поверх всех окон, горячие клавиши и фоновое
управление Яндекс Музыкой — без переключения на основное окно.

Сайт проекта: [onemorefix1337.github.io/ymhub](https://onemorefix1337.github.io/ymhub/)

## Сборка

Понадобятся:

- Visual Studio 2022+ (компонент "Desktop development with C++")
- CMake 3.20+
- [NuGet CLI](https://www.nuget.org/downloads) (или `nuget` из любого другого источника, например из комплекта Visual Studio)

YMHub встраивает WebView2 через статически линкуемый `WebView2LoaderStatic.lib`
из NuGet-пакета `Microsoft.Web.WebView2` — он не лежит в репозитории и
его нужно восстановить перед конфигурацией CMake:

```bat
nuget install Microsoft.Web.WebView2 -Version 1.0.2592.51 -OutputDirectory webview2sdk

cmake -S . -B build -A x64 -DWV2_SDK="%CD%\webview2sdk\Microsoft.Web.WebView2.1.0.2592.51"
cmake --build build --config Release
```

Готовый `YMHub.exe` появится в `build\bin\Release\`.

Версию `Microsoft.Web.WebView2` можно взять другую — главное, чтобы
`-DWV2_SDK` указывал на корень распакованного пакета (там, где лежат
папки `build/native` и `lib/`).

## Структура проекта

- `src/dll` — `YMHubDll.dll`, инжектируется в процесс Яндекс Музыки;
  управляет лайками/треком напрямую через DevTools Protocol страницы.
- `docs` — лендинг проекта (GitHub Pages).

## Лицензия / отказ от ответственности

Независимый проект, не связан с «Яндексом».
