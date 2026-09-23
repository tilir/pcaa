# pcaalib

`pcaalib` lets C and C++ programs describe PCAA cost operations without
assembling device descriptors by hand. The same library serves the bare-metal
driver and the hosted model. Platform backends send those operations to the
device, so application code need not write MMIO or SystemC transactions.

See the [API reference](../doc/pcaalib.md) for functions, views, encoding,
ownership, and versioning. The [architecture specification](../doc/arch.md)
describes what the accelerator executes.
