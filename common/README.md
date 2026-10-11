# common/ — wire protocol

`pspcap_protocol.h` is plain C and is included by both the plugin and the
client: packet header, packet types, payload structs and
`PSPCAP_PROTO_VERSION`. Any change bumps the version, and the client refuses
to talk to a plugin with a different one.
