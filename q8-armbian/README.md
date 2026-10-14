# FLTwitcher-Q8

Adaptación de FLTwitcher para dispositivos ARMv7 con Linux,
desarrollada y probada en la tablet Q8-A33 con Armbian.

## Proyecto original

Autor: jcarlosrf26

Repositorio:
https://github.com/jcarlosrf26/FLTwitcher

Este proyecto es un fork que conserva los créditos del autor
original y agrega modificaciones específicas para la Q8-A33.

## Características

- Interfaz gráfica ligera desarrollada con FLTK.
- Exploración de transmisiones en vivo de Twitch.
- Búsqueda de canales.
- Reproducción de directos mediante MPV.
- Consulta y reproducción de grabaciones (VOD).
- Sistema de favoritos persistentes.
- Visualización de canales favoritos desconectados.
- Acceso a grabaciones desde la lista de favoritos.
- Funcionamiento sin iniciar sesión en Twitch.

## Plataforma probada

- Tablet Q8-A33.
- Procesador Allwinner A33, ARMv7.
- Armbian / Debian 12 Bookworm armhf.
- Escritorio IceWM.
- Pantalla 1024x600.
- Memoria RAM aproximada: 457 MiB.

## Archivos

- fltwitcher-q8.cpp: código fuente de la adaptación.
- fltwitcher-q8: ejecutable ARMv7 armhf.

## Dependencias

FLTwitcher-Q8 utiliza:

- FLTK
- libcurl
- json-c
- X11
- MPV

El ejecutable es dinámico y requiere las bibliotecas
compatibles instaladas en el sistema.

## Instalación

Copiar el ejecutable al dispositivo ARMv7:

    mkdir -p ~/FLTwitcher-Q8
    cp fltwitcher-q8 ~/FLTwitcher-Q8/
    chmod +x ~/FLTwitcher-Q8/fltwitcher-q8

Ejecutar desde una sesión gráfica:

    ~/FLTwitcher-Q8/fltwitcher-q8

## Favoritos

Los favoritos se almacenan en:

    ~/.fltwitcherrc

## Licencia y créditos

Proyecto original: FLTwitcher, de jcarlosrf26.

La adaptación mantiene la licencia del proyecto original.
Consultar el repositorio principal para más información.

## Estado

Versión funcional probada en Q8-A33 con Armbian.
