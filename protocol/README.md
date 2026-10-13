# Protocol fixtures

Sample frames of the host/device serial protocol, one JSON file per frame. Both
sides test against the same files, so a field or method changed on one side fails
the other side's tests:

- `tools/esp32dash` (`cargo test`): the host serialises exactly these frames, and
  every method name it sends is one the device declares in `hello.json`.
- firmware (`make test-host`): the device's parsers read every field they depend
  on from these frames, and its method list equals `hello.json`.
