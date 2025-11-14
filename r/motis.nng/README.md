# motis.nng

`motis.nng` is a small helper package that talks to a local MOTIS instance
through the NNG REQ/REP IPC transport.

## Requirements

1. Build MOTIS with IPC support (enabled by default) and start the server with
   the IPC flags:
   ```bash
   ./motis server --ipc.enable=1 --ipc.address=ipc:///tmp/motis-ipc.sock
   ```
2. Install the R dependencies:
   ```r
   install.packages(c("nanonext", "jsonlite"))
   ```
3. Load the package (e.g. via `devtools::load_all("r/motis.nng")`).

## Example

```r
motis.nng::motis_connect()
metrics <- motis.nng::motis_request("/metrics", method = "GET")
motis.nng::motis_disconnect()
```

## Tests

The package ships with a basic integration test. Set the environment variable
`MOTIS_IPC_TEST=1` while a MOTIS IPC server is running to execute it.
