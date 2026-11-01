# Password Vault ESP32

Un password manager hardware basato su **ESP32-S3**. La chiavetta conserva solo un blob cifrato; cifratura e decifratura avvengono nel browser, tramite una web app che parla con il dispositivo via **Web Serial**. La master password e le chiavi non lasciano mai il PC o il telefono.

## Come funziona

```
┌─────────────────────────────┐   USB (Web Serial)   ┌──────────────────────────┐
│ Browser (web/index.html)    │ ───────────────────► │ ESP32-S3 (src/main.cpp)  │
│ - PBKDF2-SHA256 (600k iter) │   blob cifrato       │ - salva/legge il blob    │
│ - AES-256-GCM               │ ◄─────────────────── │ - slot A/B in flash      │
│ - UI, generatore password   │                      │ - CRC32 + verifica       │
└─────────────────────────────┘                      └──────────────────────────┘
```

- **Client**: deriva una chiave AES-256 dalla master password con PBKDF2-SHA256 (600.000 iterazioni, salt casuale), cifra l'intero vault con AES-256-GCM e invia al dispositivo un JSON `{v, kdf, iter, salt, iv, ct}`.
- **Firmware**: memorizza il blob così com'è, senza poterlo leggere. Usa una partizione dedicata `vault` con due slot da 128 KB scritti in alternanza: se la corrente salta durante una scrittura, l'ultima copia valida resta intatta. Ogni slot ha header con magic, numero di sequenza, lunghezza e CRC32, e viene riletto e verificato dopo ogni scrittura.

## Funzionalità

- Creazione del vault e sblocco con master password (minimo 10 caratteri)
- Aggiunta, modifica, eliminazione e ricerca delle voci
- Generatore di password casuali (rejection sampling, senza bias)
- Copia negli appunti con pulizia automatica dopo 30 secondi
- Blocco automatico dopo 5 minuti di inattività
- Cambio della master password
- Export e import del backup (il file resta cifrato)
- Tema chiaro e scuro automatico

> ⚠️ Non c'è modo di recuperare la master password. Se la perdi, i dati non sono più leggibili.

## Hardware

- Scheda **ESP32-S3-DevKitC-1** (testata con flash da 16 MB)
- Collegamento tramite la porta **USB nativa** (USB-Serial/JTAG)

## Requisiti

- [PlatformIO](https://platformio.org/) (CLI o estensione VS Code)
- Un browser che supporta Web Serial: **Chrome** o **Edge** su desktop, Chrome su Android

## Installazione del firmware

1. Clona il repository:
   ```bash
   git clone git@github.com:deste04/password-vault-esp32.git
   cd password-vault-esp32
   ```
2. In `platformio.ini` imposta `upload_port` e `monitor_port` sulla porta della tua scheda (di default è `COM4`).
3. Compila e carica:
   ```bash
   pio run -t upload
   ```
4. Opzionale, per controllare che risponda:
   ```bash
   pio device monitor
   ```
   Scrivi `PING` e premi Invio: deve rispondere `PONG`.

### Note sulla scheda

- `-DARDUINO_USB_MODE=1` e `-DARDUINO_USB_CDC_ON_BOOT=1` servono perché la seriale passi dalla porta USB nativa. Senza questi flag il monitor dà "Write timeout" e non arrivano risposte.
- Alcune flash Spansion/Infineon ignorano l'erase a settori da 4 KB fuori dai settori di parametro, e esptool fallisce con *"MD5 of file does not match data in flash"*. Lo script `erase_app.py` viene eseguito prima di ogni upload e cancella la partizione app a blocchi da 64 KB. La partizione `vault` non viene toccata, quindi riflashare il firmware non cancella le password.
- Per lo stesso motivo il firmware cancella gli slot del vault solo a blocchi interi da 64 KB.

## Uso

1. Collega la scheda via USB.
2. Apri `web/index.html` in Chrome o Edge. Funziona anche aprendolo come file locale, oppure puoi servirlo da un host HTTPS.
3. Premi **Connetti dispositivo** e scegli la porta seriale.
4. Se la chiavetta è vuota crei un nuovo vault, altrimenti lo sblocchi con la master password.

Ogni modifica viene cifrata e salvata subito sul dispositivo.

## Protocollo seriale

Un comando per riga, terminato da `\n`, a 115200 baud.

| Comando        | Risposta                                           |
|----------------|----------------------------------------------------|
| `PING`         | `PONG`                                             |
| `INFO`         | `INFO <proto> <max_bytes> <stored_bytes> <seq>`    |
| `GET`          | `DATA <blob>` oppure `EMPTY`                       |
| `BEGIN <len>`  | `READY` (inizia un upload di `<len>` byte)         |
| `CHUNK <text>` | `ACK <ricevuti>` (massimo 1024 caratteri per chunk)|
| `COMMIT`       | `OK <seq>` oppure `ERR <motivo>`                   |
| `WIPE`         | `OK 0` oppure `ERR <motivo>`                       |

Gli upload sono divisi in chunk, con un ACK per ciascuno, perché il driver USB-Serial/JTAG scarta i byte in arrivo quando il buffer RX è pieno e non ha controllo di flusso.

## Struttura del progetto

```
├── src/main.cpp       # firmware: protocollo seriale e storage A/B
├── web/index.html     # web app: UI, crittografia, Web Serial
├── partitions.csv     # tabella partizioni (factory 2 MB + vault 256 KB)
├── erase_app.py       # script pre-upload per l'erase a blocchi da 64 KB
└── platformio.ini     # configurazione PlatformIO
```

## Sicurezza

- Il dispositivo vede solo dati cifrati. Chi ruba la chiavetta deve comunque indovinare la master password, e ogni tentativo costa 600.000 iterazioni di PBKDF2.
- AES-GCM autentica i dati: se il blob viene modificato, la decifratura fallisce.
- La flash dell'ESP32 non è cifrata e il dispositivo non ha secure boot. La protezione dipende solo dalla robustezza della master password, quindi scegline una lunga.
- È un progetto personale e non ha avuto audit di sicurezza.
