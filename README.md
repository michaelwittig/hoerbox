# toniebox

## Setup env
```
export ADF_PATH=$HOME/esp/esp-adf
. $HOME/esp/esp-adf/esp-idf/export.sh
```

## Build / Flash

Am Board Boot gedrückt halten, reset kurz drücken und boot loslassen. Dann:
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

- [ ] Store volume and read value on start
- [ ] Store last play position and play from last position
- [ ] write empty file with TAG no if file is not found
- [ ] can we us the internal event system to connect I2C/RFID?
