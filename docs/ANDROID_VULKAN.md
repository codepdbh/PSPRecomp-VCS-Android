# Vulkan en Android

El backend Vulkan recibe los triángulos del GE y las texturas decodificadas,
dibuja a 480 × 272 y devuelve la imagen final a la ventana Android. La ruta
experimental tiene dos pases: el mundo en el framebuffer PSP `0x88000` y la
composición hacia `0x178000`. La composición muestrea la imagen del mundo en la
GPU, sin leer sus píxeles de la RAM emulada.

## Estado

`Backend=Vulkan` está instalado para pruebas en el S25 Ultra. Una captura
anterior mostraba solo cielo y siluetas negras: la profundidad invertida se
limpiaba a `1.0` en vez de `0.0`, y Vulkan aplicaba mezcla alfa incluso a
dibujos opacos. Con ambas correcciones, la captura del 24 de septiembre de
2026 muestra geometría y texturas del mundo. La imagen aún presenta errores
visibles y no se considera correcta. `Backend=Software` sigue siendo la opción
de recuperación. El menú de pausa intenta usar temporalmente el framebuffer
software cuando no hay dibujos del mundo; falta verificarlo visualmente.

La salida aún hace una lectura de GPU a CPU cada fotograma para mostrarla en
`ANativeWindow`; falta presentar mediante una swapchain. También faltan filtros,
mipmaps, modos de mezcla, máscaras de color y realimentación entre otros
framebuffers equivalentes al PSP. Por eso el modo Vulkan sigue siendo
experimental y no es el valor por defecto.

## Configuración

La app lee `VCSNative.ini` en su directorio privado `files/`:

```ini
[Timing]
FrameRate=60

[Rendering]
Backend=Software
```

`Backend=VulkanPreview` presenta la imagen Vulkan para inspección, mientras
mantiene el rasterizador software para los framebuffers PSP. Consume más CPU.
`Backend=Vulkan` intenta dibujar el mundo en GPU y usa el fallback software
para menús; úsalo solo para pruebas hasta completar la comparación visual.

## Mediciones del S25 Ultra

Con el objetivo anterior de 240 Hz, la escena pesada bajó a unos 13 vblanks/s
y AAudio registró varios underruns por segundo. A 60 Hz, el audio aguantó
mejor, pero el mundo seguía rasterizándose en CPU. Los registros del GE
identificaron `0x88000` como framebuffer del mundo y `0x178000` como el de
pantalla; este último contiene unos 64 dibujos de composición por fotograma.
En la prueba con las correcciones, el registro reportó aproximadamente
59–60 vblanks/s tras cargar la partida, con cero underruns de audio nuevos
durante el intervalo observado. Esta medición no equivale aún a 60 fotogramas
del juego por segundo ni valida la imagen.

Los shaders fuente están en `profiles/vcs/shaders/vulkan/`. El script
`profiles/vcs/tools/embed_vulkan_shaders.py` integra sus binarios SPIR-V en
`profiles/vcs/host/ge_gpu_vulkan_shaders.hpp`.
