# Changelog

Tutte le modifiche rilevanti a questo progetto saranno documentate in questo file.

Il formato è basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/).

## [Unreleased]

## [1.0.0] - 2026-09-13

### Added
- Release iniziale v1.0.0 del sistema `fast_cam_capture`.
- Architettura client/server IPC zero-copy via Unix domain socket e `dma-buf` file descriptor passing.
- Shared library `libfast_cam_client.so` con API C/C++ ad alto livello (`fast_cam_client_create`, `fast_cam_client_poll_frame`, `fast_cam_get_version`).
- Supporto cattura multi-camera sincronizzata (SA8155P DiLink 5.0).
- Hardening dei binari, offuscamento stringhe sensibili e anti-debugging.
- Adozione della licenza **GNU General Public License v3 (GPLv3)** con attribuzione del copyright all'autore originale (**Francesco D'Offizi**).
- Creazione del file `LICENSE` con il testo ufficiale GPLv3.
- Intestazioni di licenza e identificatori standard SPDX (`SPDX-License-Identifier: GPL-3.0-or-later`) su tutti i file sorgente (`.c`, `.cpp`) e header (`.h`).
- Creazione del file `.gitignore` per escludere build artifacts, dump di test e binari locali.

### Removed
- Rimozione di binari proprietari estratti da terze parti (`qcarcam_test`, `libais_test_util_proprietary.so`, `libhook_qcarcam.so`, `lib/libais_client.so`) per conformità legale al rilascio pubblico.
- Pulizia dei dump multimediali e registrazioni raw di test (`output/`).


