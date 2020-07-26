# toniebox

## Setup env
```
/bin/bash
export ADF_PATH=$HOME/esp/esp-adf
. $HOME/esp/esp-adf/esp-idf/export.sh
```

## Build / Flash

Am Board <Boot> button gedrückt halten, <Reset> button kurz drücken und <Boot> loslassen.

Dann:
```
make flash monitor -j5
```

## ESP ADF version used

Inside `~/esp/esp-adf`:

```bash
git fetch
git checkout v2.0
git submodule update --init --recursive
```

## Features

* Auge kurz drücken: lauter/leiser
* Auge lang drücken: spulen (je länger, desto mehr)
* Beide Augen lange drücken: MP3 zurücksetzen auf Anfang
* Wenn ein RFID Tag aufgelegt wird zu dem es keine MP3 gibt wird eine Leere Datei auf der SK Karte angelegt.

## TODO

- [x] Store last play position and play from last position
- [x] Volume max begrenzen
- [x] Store volume and read value on start
- [x] turn off if no music is played for x minutes
- [x] write empty file with TAG no if file is not found
- [x] Reset last played position by pressing both keys long?
- [x] fast forward / rewind
- [ ] can we us the internal event system to connect I2C/RFID?
