# iofailuresim

Simulator of hard resets and fsync failures for applications that write append-only files. Assumes the application uses the C standard library's `write()`, `fdatasync()`, and `fsync()` for writing and syncing data.

## Compile

`gcc -std=c99 -fPIC -shared -o libiofailuresim.so iofailuresim.c -ldl`

## Run

`LD_PRELOAD=./libiofailuresim.so <your application>`

## Configure

- `CRASH_FAILURE_ONE_IN=<n>`: During a sync operation, crash the process immediately with a probability of 1/n. All unsynced writes are lost since they are buffered in process memory.
- `SYNC_FAILURE_ONE_IN=<n>`: A sync operation will return failure with a probability of 1/n. All unsynced writes for the file are dropped to simulate the failure.
- `CRASH_AFTER_SYNC_FAILURE=<0|1>`: If set to one, the program will kill itself some time after the first simulated sync failure. It does not happen immediately to allow the system to get itself into a weird state in case it doesn't handle sync failures properly.
