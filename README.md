<p align="center">
  <img src="assets/branding/icono.png" alt="Icono de VCS Android" width="180" />
</p>

<h1 align="center">PSPRecomp · Vice City Stories</h1>

<p align="center">Port experimental de Grand Theft Auto: Vice City Stories para Android ARM64 y Windows, basado en recompilación estática del código de PSP.</p>

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" />
  <img alt="Android ARM64 Vulkan" src="https://img.shields.io/badge/Android-ARM64%20%C2%B7%20Vulkan-3DDC84?logo=android&logoColor=white" />
  <img alt="Windows DirectX 12" src="https://img.shields.io/badge/Windows-DirectX%2012-0078D4?logo=windows&logoColor=white" />
  <img alt="Estado beta" src="https://img.shields.io/badge/estado-beta%200.2.0-orange" />
</p>

> **Estado actual (beta 0.2.0):** el juego se juega en Android con render por GPU (Vulkan), en pantalla completa widescreen, con audio completo (incluidas las voces de las cinemáticas) y con guardado de estado. Todavía hay detalles pendientes (ver [Problemas conocidos](#problemas-conocidos)).

## El proyecto

[PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) traduce el código Allegrex de la PSP a C++ nativo. Este repositorio añade el perfil de VCS, los servicios de plataforma para Android y Windows, y la app Android. El código del juego se recompila; los datos del juego se leen de una copia propia del usuario.

## Instalación en Android

### 1. Requisitos

- Android 8.0 o superior, procesador **ARM64** y soporte Vulkan (probado en un Samsung Galaxy S25 Ultra).
- Tu propia copia de **GTA: Vice City Stories para PSP (USA, ULUS10160)**, preparada con [setup_vcs.ps1](setup_vcs.ps1). Ese paso extrae los datos y genera el ELF descifrado.

### 2. Copia los datos del juego a la carpeta `VCS`

Los datos van en una carpeta llamada exactamente **`VCS`** (en mayúsculas) en la **raíz del almacenamiento interno** del teléfono, es decir, `/storage/emulated/0/VCS`. Dentro tiene que quedar la carpeta `PSP_GAME` del juego:

```text
Almacenamiento interno/
└── VCS/
    └── PSP_GAME/
        ├── SYSDIR/
        │   └── EBOOT_DECRYPTED.ELF
        └── USRDIR/
            └── RUNDATA/ …   (resto de los datos del juego)
```

Puedes copiarla con un explorador de archivos, por USB o con ADB:

```powershell
adb shell mkdir -p /storage/emulated/0/VCS
adb push --sync profiles\vcs\game\PSP_GAME /storage/emulated/0/VCS/
```

Si la app no encuentra `VCS/PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF`, lo avisa en pantalla al abrirla.

### 3. Instala y abre la app

1. Descarga el APK desde [Releases](https://github.com/codepdbh/PSPRecomp-VCS-Android/releases) e instálalo.
2. Al abrirla por primera vez, concede el permiso **"Acceso a todos los archivos"**; sin él no puede leer la carpeta `VCS`. Luego vuelve a abrir la app.
3. La primera carga tarda un poco más.

### Guardado de estado (botón 💾 junto a START)

Guarda el juego completo en el instante exacto —misión, cinemática, posición— y lo recupera después. Es independiente de las partidas del propio juego.

- Hay **3 ranuras**; cada una muestra la fecha y hora de lo guardado.
- Toca una ranura para **Guardar aquí** / **Sobrescribir** o **Cargar**.
- También está en ⚙ → **Guardado de estado…**.
- Los estados se guardan dentro de la app (`files/savestates`) y se pierden al desinstalarla.

### Ajustes (botón ⚙ en pantalla)

- **Guardado de estado:** las mismas ranuras que el botón 💾.
- **Resolución interna:** Rendimiento (por debajo de HD, la más fluida), HD, Full HD o nativa de la pantalla. Se aplica al reiniciar la app.
- **Editar posición de controles:** arrastra cada grupo de botones y ajusta su tamaño con − / +.
- **Restablecer controles:** vuelve al diseño original.

La configuración avanzada vive en `VCSNative.ini`, dentro de los archivos privados de la app.

### Controles

- **Táctil:** joystick flotante (aparece donde apoyas el pulgar), cruceta, △ ○ ✕ □, L, R, SELECT y START.
- **Mando Bluetooth/USB:** A = ✕, B = ○, X = □, Y = △, L1/L2 = L, R1/R2 = R, Start, Select, cruceta y stick izquierdo.
- **Teclado y ratón:** WASD para moverse (Alt para caminar), Espacio, Shift, F/Enter, Q/E, H, flechas, Esc (pausa) y Tab; clic izquierdo dispara, clic derecho apunta y el botón central mira atrás.

Al usar un mando, un teclado o un ratón, los controles táctiles se ocultan; vuelven al tocar la pantalla.

El radar está arriba a la izquierda, donde no lo tapa el pulgar. Si el botón L te lo tapa, muévelo con ⚙ → **Editar posición de controles**.

### Problemas conocidos

- En resoluciones altas el juego no siempre llega a velocidad completa. Si va lento, baja a HD o a Rendimiento desde ⚙.
- En algunos puntos (el icono de guardado del juego, el final de algunas misiones) la pantalla puede quedarse en negro. Se está investigando; mientras tanto, usa el guardado de estado antes de esos momentos para no perder avance.
- Los videos de introducción se saltan (pantalla negra): falta el decodificador de video para Android.
- El ratón todavía no mueve la cámara; sus botones sí funcionan.
- El APK beta está firmado con una clave de depuración.

## Compilar

### Android

Requisitos: Android SDK, NDK `28.2.13676358`, CMake `3.22.1`, Java compatible con Gradle 8.11.1 y un dispositivo ARM64 autorizado para ADB. Desde PowerShell, en la raíz del repositorio:

```powershell
./build_android.ps1
./run_android.ps1
```

Consulta [Vulkan en Android](docs/ANDROID_VULKAN.md) y el [análisis de portabilidad](docs/ANDROID_PORT_ANALYSIS.md) para los detalles técnicos.

### Windows

Requisitos: Visual Studio 2022 con C++, Windows SDK, CMake y los archivos de una copia propia del juego.

```powershell
./build_vcs.ps1
./run_vcs.ps1
```

La preparación local está en [setup_vcs.ps1](setup_vcs.ps1) y [prepare_game.ps1](profiles/vcs/tools/prepare_game.ps1).

## Datos y distribución

El repositorio y los APK de Releases contienen **solo el programa**, sin ningún dato del juego. No subas ISOs, EBOOTs, ELF descifrados, partidas ni recursos comerciales: cada usuario debe aportar sus propios archivos.

El framework PSPRecomp usa licencia MIT. Las licencias y avisos adicionales del perfil están en [THIRD_PARTY.md](profiles/vcs/THIRD_PARTY.md). Proyecto comunitario no oficial, sin afiliación con Rockstar Games ni Sony Interactive Entertainment.
