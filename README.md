# fast_cam_capture (v1.0.0)

Motore ad alte prestazioni e zero-copy per la cattura, streaming e condivisione inter-processo (IPC) dei flussi telecamere per veicoli BYD basati su SoC Qualcomm SA8155P (DiLink 5.0).

Sviluppato da **Francesco D'Offizi** e rilasciato sotto licenza **GNU General Public License v3 (GPLv3)**.

---

## Panoramica

`fast_cam_capture` si interfaccia direttamente con il sottosistema AIS (Automotive Imaging Subsystem) Qualcomm `/vendor/lib64/libais_client.so` del veicolo, bypassando il layer Java/HAL di Android. 

Fornisce un'architettura **client/server zero-copy**: il server cattura i frame hardware in memoria e inoltra i file descriptor dei buffer DMA (`dma-buf`) tramite Unix Domain Socket ai client consumer (applicazioni di recording, bridge Flutter/React Native, pipeline OpenCV/AI). I consumatori mappano direttamente la memoria RAM senza alcuna operazione di `memcpy`.

---

## Architettura del Sistema

```text
 +-------------------------------------------------------------+
 |              SA8155P Hardware Cameras (4x UYVY)             |
 |             Front (0), Right (1), Rear (2), Left (3)        |
 +-------------------------------------------------------------+
                               |
                               v
               [/vendor/lib64/libais_client.so]
                               |
                               v
 +-------------------------------------------------------------+
 |                   fast_cam_capture (Server)                 |
 |  - Inizializzazione AIS & Configurazione canali             |
 |  - Ricezione frame a 30 FPS                                 |
 |  - Unix Domain Socket Server (/data/local/tmp/fast_cam.sock)|
 +-------------------------------------------------------------+
                               |
       Unix Domain Socket (SCM_RIGHTS: DMA-BUF FD Passing)
                               |
            +------------------+------------------+
            |                                     |
            v                                     v
 +----------------------+             +----------------------+
 | fast_cam_client (CLI)|             | libfast_cam_client.so|
 | (Test & Benchmarking)|             | (Shared Library API) |
 +----------------------+             +----------------------+
                                                  |
                                                  v
                                      +----------------------+
                                      | Consumer App / UI /  |
                                      | AI Pipeline          |
                                      +----------------------+
```

---

## Componenti del Progetto

1. **`fast_cam_capture`** (Server Daemon):
   - Si aggancia alle telecamere a livello nativo QCarCam.
   - Gestisce la sincronizzazione e l'esposizione dei buffer DMA tramite Unix socket.
   - Supporta sia l'output diretto zero-copy su IPC che il compositing 2x2 su file.
   - Include hardening dei simboli, offuscamento stringhe (`obfuscate.h`) e anti-debugging.

2. **`libfast_cam_client.so`** (Client Library C/C++):
   - Shared library C/C++ ad alto livello pensata per integrazione diretta in app native o binding FFI (es. Flutter/Dart, Python, Go).
   - Espone API intuitive: `fast_cam_client_create()`, `fast_cam_client_connect()`, `fast_cam_client_poll_frame()`, `fast_cam_client_release_frame()`, `fast_cam_get_version()`.

3. **`fast_cam_client`** (CLI Consumer di Test):
   - Utility di riga di comando per validazione, benchmark FPS e test dell'IPC zero-copy.

---

## Struttura del Repository

```text
.
├── .gitignore
├── CHANGELOG.md
├── LICENSE
├── Makefile
├── README.md
├── include/
│   ├── fast_cam_bridge.h    # Header pubblico API C per la shared library
│   ├── fast_cam_ipc.h       # Protocollo binario IPC e costanti
│   ├── fd_passing.h         # Helper SCM_RIGHTS per passaggio file descriptor
│   ├── obfuscate.h          # String XOR decryptor e anti-debug
│   └── qcarcam_types.h      # Definizioni e strutture QCarCam SA8155P
└── src/
    ├── fast_cam_bridge.cpp  # Implementazione client bridge (shared library)
    ├── fast_cam_client.c    # Consumer CLI di test
    └── main.c               # Demone server di cattura
```

---

## Compilazione

### Prerequisiti
- **Android NDK** (r26b o superiore raccomandato).
- Architettura target: `aarch64-linux-android` (API level 30 / Android 11).

### Build dei binari
```bash
make all
```

I binari e le librerie compilate saranno generati nella directory `build/`:
- `build/fast_cam_capture`
- `build/fast_cam_client`
- `build/libfast_cam_client.so`

### Hardened Release Build
Per generare binari pronti alla distribuzione con rimozione aggressiva di tutti i simboli ELF e tabelle di debug:
```bash
make release
```

---

## Utilizzo

### 1. Avvio del Demone sul dispositivo
Eseguire sul veicolo via ADB con privilegi di root:
```bash
# Avvio in ascolto su socket IPC per tutte e 4 le telecamere
/data/local/tmp/fast_cam_capture --all
```

### 2. Esecuzione del Client di Test
```bash
/data/local/tmp/fast_cam_client --time 10
```

### 3. Integrazione della Libreria C/C++
```c
#include "fast_cam_bridge.h"

FastCamClientCtx* ctx = fast_cam_client_create();
if (fast_cam_client_connect(ctx, NULL)) {
    FastCamFrame frame;
    if (fast_cam_client_poll_frame(ctx, &frame, 100)) {
        // frame.pixels contiene il puntatore diretto in RAM (UYVY 1920x1300)
        // Zero-copy, nessun overhead di copia
        
        fast_cam_client_release_frame(ctx, frame.cam_id);
    }
}
fast_cam_client_destroy(ctx);
```

Per la guida dettagliata all'integrazione sul veicolo e nei moduli Android JNI (incluso Overdrive), consulta la [Guida all'integrazione (docs/INTEGRATION.md)](docs/INTEGRATION.md).

---

## Licenza

Questo progetto è distribuito sotto licenza **GNU General Public License v3 (GPLv3)**. Consultare il file [LICENSE](LICENSE) per i termini completi.

Qualsiasi modifica o prodotto derivato distribuito a terzi deve obbligatoriamente rilasciare i codici sorgenti sotto la medesima licenza GPLv3, preservando l'attribuzione all'autore originale.

Copyright (C) 2026 Francesco D'Offizi
