# Changelog

Tutte le modifiche e gli sviluppi del progetto sono annotati in questo file.
I rilasci verranno versionati in occasione del Version Bump.

## [Unreleased] - 2026-09-02

### Aggiunto
- Analisi architetturale dei sottosistemi hardware/software dell'infotainment BYD DiLink 5.0 (Qualcomm SA8155P, Android 11 AAOS, Hypervisor QNX/Gunyah).
- Identificazione della pipeline video a 8 canali (`/dev/video51`..`/dev/video58`, `ais_v4l2loopback_config.xml`).
- Diagnosi del collo di bottiglia a 3-4 FPS di `qcarcam_test` (de-interlacing software CPU, display lock e buffer starvation).
- Progettazione e implementazione del demone nativo C `fast_cam_capture`:
  - Interfaccia diretta dinamica con `libais_client.so` e HAB (`/dev/hab`).
  - Ring buffer circolare a 5 buffer ION da 1920x1300 UYVY (4.992.000 byte).
  - Modalità Zero-Copy con rilascio immediato dei frame per evitare starvation dell'ISP.
  - Benchmark FPS e latency in tempo reale.
  - Flag `--copy` per confrontare le prestazioni con e senza memcpy software.
  - Flag `--dump` per catturare singoli frame raw di test.
- Diagnosi del collo di bottiglia IPC dell'hook preesistente:
  - Rilevato stream socket con 36.000 syscall/s `sendto()` di 600 MB/s di dati grezzi che degradavano i 30 FPS hardware a 4.3 FPS sul consumatore Java.
- Architettura Zero-Copy Inter-Process Communication (IPC):
  - Creata interfaccia IPC `fast_cam_ipc.h` basata su socket UNIX locale (`/data/local/tmp/fast_cam.sock`).
  - Condivisione dei file descriptor ION `dma-buf` tramite `sendmsg` con controllo ausiliario `SCM_RIGHTS` (`include/fd_passing.h`).
  - Notifica istantanea lightweight dei frame (messaggio binario compatto di 32 byte anziché copia di 5 MB).
  - Sviluppato client consumer di test `fast_cam_client` (`src/fast_cam_client.c`) per mappatura diretta in RAM condivisa senza carico CPU.
- Motore Multi-Camera Indipendente (4 Canali Paralleli):
  - Supporto per apertura concorrente di tutti i 4 sensori AVM (0: Frontale, 1: Destra, 2: Posteriore, 3: Sinistra).
  - Gestione di ring buffer ION dedicati per ciascun canale con passaggio aggregato dei 20 FD via IPC.
  - Risoluzione definitiva del formato pixel QCarCam (`0x7080102` = UYVY nativo) e heap ION `0x2000000` (system heap `1u << 25`).
  - Raggiunto framerate di **29.98 FPS** su telecamera singola e **122.25 FPS aggregati** su 4 canali contemporanei.
- Modulo di Registrazione Video e Composizione:
  - Compositor 2x2 SIMD-friendly in spazio colore UYVY (4 quadranti: Front, Right, Left, Rear) a 30 FPS unificati su telaio 1920x1300.
  - Registrazione e codifica H.264 MP4 a **30.00 FPS effettivi**:
    - `cam0_5s_30fps.mp4` (Telecamera 0 frontale, 5 secondi a 30 FPS reali).
    - `4cam_2s_30fps.mp4` (Griglia 2x2 delle 4 telecamere, 2 secondi a 30 FPS).
- Pacchetto di Integrazione per i Maintainer di Overdrive (Binary Distribution):
  - Redatto `integration_plan.md` con guida completa all'integrazione del demone e del bridge client C++ in Overdrive.
  - Compilata la libreria condivisa client `build/libfast_cam_client.so` e definito l'header pubblico `include/fast_cam_bridge.h` per consentire ai maintainer di integrare lo stream a zero-copy senza esporre i sorgenti del demone.
  - Pulizia completata su target `/data/local/tmp/`: rimossi i video raw temporanei (2,4 GB liberati) e i file obsoleti (`qcarcam_test`, `libhook_qcarcam.so`, `dilink5_cam_sidecar`, `4cam.xml`), mantenendo esclusivamente il nuovo binario nativo `fast_cam_capture`.
- Hardening, Protezione Anti-Reverse Engineering & Versioning Git:
  - Inizializzato repository Git con due release versionate:
    - `v1.0.0-clean`: Versione sorgente pulita e non offuscata.
    - `v1.0.0-hardened`: Versione di produzione protetta e offuscata.
  - Implementato modulo di sicurezza `obfuscate.h`:
    - Cifratura XOR delle stringhe a runtime con barriere volatili (zero riferimenti a `qcarcam`, `libais`, `/dev/ion`, socket nei comandi `strings`).
    - Calcolo opaco a runtime delle costanti hardware proprietarie (`0x7080102`, `0x2000000`, `0x4643414D`).
    - Hook di protezione anti-debug (`/proc/self/status` TracerPid check).
    - Strip aggressivo di tutti i simboli interni con `llvm-strip --strip-all --discard-all` nel target `make release`.
    - Validata l'esecuzione sul veicolo reale a 30 FPS su telecamera singola e 121 FPS su 4 telecamere.
