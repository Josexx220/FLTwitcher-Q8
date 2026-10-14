# FLTwitcher

Cliente de Twitch para **FLinux (Tiny Core Linux)**, escrito desde cero en **C++ con FLTK**.

Sin cuenta, sin navegador: muestra los directos, busca canales y categorías, y reproduce con **mplayer**.

## Ventana principal

!Ventana principal

### Favoritos

!Sección de favoritos

El pulgar 👍 marca un canal como favorito. Los favoritos se guardan en `~/.fltwitcherrc`.

### Reproduciendo

!Canal seleccionado para reproducir

## Ventana principal

!Ventana principal en FLinux oficial

FLTwitcher 0.1 corriendo en el FLinux oficial de Nicolás Longardi. Los círculos lila neón (`#A970FF`) indican los canales en vivo.

### Búsqueda

!Ejemplo de búsqueda

La búsqueda abarca toda la plataforma por canal y por categoría. Los resultados en vivo aparecen arriba ordenados por espectadores; los apagados, debajo.

### Favoritos

!Sección de favoritos

El pulgar 👍 blanco marca un canal como favorito; al activarlo se vuelve violeta fluorescente. Los favoritos se guardan en `~/.fltwitcherrc`.

### Ayuda

!Ventana de ayuda

El botón de información (i) abre la ventana de Ayuda con pestañas: qué es FLTwitcher, cómo usarlo y créditos.

## Qué es

FLTwitcher es un cliente ligero de Twitch pensado para equipos de 32 y 64 bits con pocos recursos, como la Dell Inspiron 1545 con FLinux-JC. No necesita cuenta de Twitch ni navegador: se conecta a la API web pública de Twitch, la misma que usa twitch.tv para cualquier visitante, y reproduce los directos a través de un tubo `ffmpeg → mplayer`.

## Cómo funciona

1. **API GraphQL sin cuenta**: consulta ` con el Client-ID público que la web de Twitch reparte en su JavaScript. Sin OAuth, sin registro.
2. **Lista de directos**: pide los 20 directos con más espectadores (`streams(first:20)`).
3. **Búsqueda**: por canal exacto (`user(login)`) o por categoría/juego (`game(name)`); las sugerencias al escribir solo muestran canales en vivo.
4. **Token de reproducción**: solicita el `streamPlaybackAccessToken` para el canal elegido.
5. **Usher (HLS)**: con el token descarga el master `m3u8` de `usher.ttvnw.net`.
6. **Variante por calidad**: elige la variante según la calidad seleccionada (160p, 240p, 360p, 480p, 720p; 360p por defecto).
7. **Tubo ffmpeg → mplayer**: `ffmpeg` demuxa el HLS y entrega MPEG-TS por tubería a `mplayer -zoom -vo x11`. La tecla **f** alterna la pantalla completa, también en Xvesa.

La red corre en hilos `pthread` con reintentos automáticos; la interfaz se actualiza con `Fl::awake`.

## Cómo está hecho

- **Lenguaje**: C++ con **FLTK 1.3** (interfaz ligera, sin dependencias pesadas).
- **Escrito desde cero**: no reutiliza otro proyecto como base.
- **Arquitecturas**: paquetes `.tcz` para 32 bits (`fltwitcher32.tcz`) y 64 bits (`fltwitcher.tcz`).
- **Red**: `libcurl` con tiempos de espera largos, 2 reintentos y solo IPv4.
- **JSON**: `libjson-c` para las respuestas de la API.
- **Favoritos**: botón 👍 por canal, persistidos en `~/.fltwitcherrc`.
- **Temas**: modo oscuro y modo claro conmutables.

## Instalación

Descarga del repositorio estos 3 archivos (elige 32 o 64 bits según tu sistema):

- `fltwitcher32.tcz` (o `fltwitcher.tcz` para 64 bits)
- `fltwitcher32.tcz.dep`
- `fltwitcher32.tcz.md5.txt`

Verifica la integridad:

```sh
md5sum -c fltwitcher32.tcz.md5.txt
```

Instala:

```sh
tce-load -i fltwitcher32.tcz
```

El script `tce.installed` agrega el icono al **wbar** automáticamente. Para cargarlo en cada arranque, añádelo a `tce/optional/onboot.lst`.

Dependencias: `fltk-1.3.tcz`, `curl.tcz`, `json-c.tcz`, `mplayer-cli.tcz`, `ffmpeg4.tcz`.

## Créditos y agradecimientos

- **Twitch** — por la API web pública que permite el cliente sin cuenta.
- **FLTK** (Bill Spitzak y equipo) — toolkit ligero en C++.
- **Tiny Core Linux** (Robert Shingledecker) — base de FLinux.
- **MPlayer** y **FFmpeg** — reproducción de vídeo.
- **Nicolás Longardi** — creador de FLinux.
- **Facundo Adorno** — creador de FLTube, inspiración del proyecto.

## Licencia

FLTwitcher es software libre, publicado bajo la **Licencia Pública General de GNU, versión 3 (GPL-3.0)**.

Puedes redistribuirlo y modificarlo bajo los términos de la GPL-3.0 publicada por la Free Software Foundation. Este programa se distribuye con la esperanza de que sea útil, pero SIN NINGUNA GARANTÍA, ni siquiera la garantía implícita de COMERCIABILIDAD o IDONEIDAD PARA UN PROPÓSITO PARTICULAR. Consulta la GPL-3.0 para más detalles.

## Autor

- **Juan Carlos** (jcarlosrf26) — especificación, diseño y pruebas en Dell Inspiron 1545.
