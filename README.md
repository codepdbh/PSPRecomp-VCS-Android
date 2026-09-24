<p align="center">
  <img src="assets/branding/icono.png" alt="VCS Android project icon" width="210" />
</p>

<h1 align="center">PSPRecomp · Vice City Stories</h1>

<p align="center">
  Recompilación estática experimental de <strong>Grand Theft Auto: Vice City Stories</strong>
  para PSP, con un host nativo de escritorio y una base de port para Android ARM64.
</p>

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" />
  <img alt="Windows" src="https://img.shields.io/badge/Windows-DX12-0078D4?logo=windows&logoColor=white" />
  <img alt="Android ARM64" src="https://img.shields.io/badge/Android-ARM64%20bring--up-3DDC84?logo=android&logoColor=white" />
  <img alt="Experimental" src="https://img.shields.io/badge/estado-experimental-orange" />
</p>

> **Estado:** el host de Windows alcanza el juego; el port Android está en su
> primera fase. La app Android actual es una pantalla de bring-up que verifica
> JNI y el núcleo ARM64: todavía no ejecuta ni renderiza el juego.

## Qué es

Este proyecto usa [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp),
un framework de recompilación estática para PSP. Traduce el código Allegrex del
juego a unidades C++ y lo ejecuta con un runtime nativo, manteniendo la lógica
específica de VCS en `profiles/vcs/`.

La rama Android comparte el runtime, la memoria PSP y el código AOT. La capa de
plataforma aún necesita servicios Android de ciclo de vida, almacenamiento,
entrada y audio, además de un backend gráfico ARM64.

## Progreso

| Área | Estado |
| --- | --- |
| Runtime C++ para Windows | Compila y arranca con datos locales del usuario |
| Compilación ARM64 del núcleo | Compila con Android NDK y Clang |
| App Android de desarrollo | Instalada y abierta en un Samsung Galaxy S25 Ultra |
| Perfil VCS completo en Android | En desarrollo; faltan dependencias y servicios de plataforma |
| Juego renderizado en Android | Pendiente |

Consulta el [análisis de portabilidad Android](docs/ANDROID_PORT_ANALYSIS.md) para
ver el mapa de componentes y los bloqueos actuales.

## Compilar y abrir la app Android de desarrollo

Requisitos: Android SDK, Android NDK `28.2.13676358`, CMake `3.22.1`, Android
Studio (JBR) y un dispositivo ARM64 autorizado en ADB.

Desde PowerShell, en la raíz del repositorio:

```powershell
./build_android.ps1
./run_android.ps1
```

Esto crea e instala un APK de desarrollo local. **No se publican APKs ni se han
creado releases.** La pantalla confirma el enlace JNI y la inicialización de la
memoria PSP; no es todavía una versión jugable.

## Compilar y ejecutar en Windows

Requisitos: Visual Studio 2022 con C++ y Windows SDK, CMake y los archivos de
juego de una copia propia.

```powershell
./build_vcs.ps1
./run_vcs.ps1
```

El perfil requiere un ELF descifrado que coincida con el hash configurado. La
preparación local está automatizada en [`setup_vcs.ps1`](setup_vcs.ps1) y
[`profiles/vcs/tools/prepare_game.ps1`](profiles/vcs/tools/prepare_game.ps1).

## Datos del juego

Los archivos de juego **no forman parte de este repositorio ni del APK**. Usa
una extracción de tu propia copia y conserva esos datos en almacenamiento local.
No subas ISOs, EBOOTs, ELF descifrados, partidas ni recursos comerciales a GitHub.

## Siguientes pasos

- Sustituir la pantalla de prueba por el ciclo de vida y los servicios Android.
- Separar el perfil VCS de la implementación DirectX 12 de Windows.
- Añadir un backend Vulkan ARM64 y una ruta de audio/media compatible con Android.
- Importar los datos locales mediante almacenamiento privado o el selector de
  carpetas de Android.
- Probar arranque, menú, juego, pausa/reanudación y controles en hardware real.

## Licencias

El framework PSPRecomp está bajo MIT. El perfil VCS y sus dependencias pueden
tener avisos y licencias propios; consulta
[`profiles/vcs/THIRD_PARTY.md`](profiles/vcs/THIRD_PARTY.md) y conserva los
avisos junto a cada componente.

Este es un proyecto comunitario no oficial y no está afiliado con Rockstar
Games ni Sony Interactive Entertainment.
