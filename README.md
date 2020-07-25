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


## TODO

- [x] Store last play position and play from last position
- [ ] volume max setting
- [x] Store volume and read value on start
- [ ] turn off if no music is played for x minutes
- [ ] write empty file with TAG no if file is not found
- [ ] Reset last played position by pressing both keys long?
- [ ] fast forward / rewind
- [ ] can we us the internal event system to connect I2C/RFID?
