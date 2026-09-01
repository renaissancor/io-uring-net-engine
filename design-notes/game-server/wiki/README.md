# `wiki/` — per-source-file design specs for `iouring-net-server`

Mirrors the planned `src/` layout. Each markdown file is the spec for
one source-tree concept (file, class, or codegen artifact). For
cross-cutting design see `docs/`.

## Layout

```
wiki/
├── server/                  # server-side application code
│   ├── main.md              # server entry point (server/main.cpp)
│   ├── lifecycle.md         # startup, bind, listen, accept, shutdown
│   ├── dispatch.md          # packet-id → handler routing
│   └── handlers.md          # concrete handle_X free-function handlers
│
├── client/                  # client-side application code
│   └── main.md              # client entry point (client/main.cpp)
│
├── proto/                   # wire schema
│   └── packets.md           # packets.json structure + invariants
│
├── codegen/                 # build-time generators
│   └── pipeline.md          # rpc_gen / stub_gen / proxy_gen scripts
│
└── runtime/                 # how the product wires into the library
    └── integration.md       # find_package + reactor/session bring-up
```

## Conventions

Same as the library's `wiki/`:
- One concept per file, named after the source artifact it specifies
  (e.g. `dispatch.md` ↔ `server/dispatch.{h,cpp}`).
- Each spec opens with **Purpose**, then **Interface**, then
  **Invariants**, then **Implementation notes**, then
  **Reference origin** (`path:line` citations into
  `~/CLionProjects/SelectServer/TestSerialize/` or
  `~/CLionProjects/IOCP_Rookiss/Server/`).
- Specs cite the library's wiki when they consume a library type
  (e.g. `iouring_net::session`); they do not duplicate library design.
