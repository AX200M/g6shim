# xgameruntime

## Exports

| Export | Behaviour |
|---|---|
| `XGameRuntimeInitialize`, `XGameRuntimeUninitialize` | Logged; no state |
| `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2` | Version, build and flags logged and ignored |
| `UninitializeApiImpl` | Logged |
| `QueryApiImpl` | Looks up `(provider, interface)`; unknown pairs log once and return `0x80070032` |
| `XErrorReport` | Logs the code and message |

## Runtime objects

Static objects; `AddRef` and `Release` are no-ops that return 1. Slot 0 (`QueryInterface`) is unimplemented on every object and returns `0x80004001`.

| Area | Provider | Interface | Object |
|---|---|---|---|
| task queues | `073b7dcb-1fcf-4030-94be-e3c9eb623428` | the provider | `task queue` |
| user | `01acd177-91f9-4763-a38e-ccbb55ce32e0` | `eb9bf948-18dc-4d82-bbcc-40e0a809c4c0` | `user` |
| user | `01acd177-91f9-4763-a38e-ccbb55ce32e0` | the provider | `user` |
| user gamertag | `01acd177-91f9-4763-a38e-ccbb55ce32e0` | `cef4fac0-7676-4a94-a119-4c43f9eb5b74` | `user gamertag` |
| user devices | `7d824997-10dc-45ab-86b7-2737767c0bf1` | `0cc6a956-e7e1-4fdf-9341-9d5da649ebc8` | `user devices` |
| system | `e349bd1a-fc20-4e40-b99c-4178cc6b409f` | `6fd71f09-7513-49f0-89bc-bfaf5df6f852` | `system` |
| system | `e349bd1a-fc20-4e40-b99c-4178cc6b409f` | `1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf` | `system` |
| analytics | `b884675d-b738-4a9c-815d-9a9a1e0c6c9b` | the provider | `analytics` |
| title | `973a344e-24bf-4d0f-8457-56c534892b29` | `2549f142-6419-4a06-97b5-931aab7c2f34` | `title` |
| features | `8836fe87-edb9-4fe3-8dad-05f0d2cd5b40` | the provider | `features` |
| invites | `0651aae2-4012-4077-bf84-8b9097090e2c` | the provider | `invites` |
| events | `bbfbdcc7-bfe7-409b-a5ca-edf054960b4d` | the provider | `events` |

## Method slots

Byte offsets from the object pointer.

| Object | Slots |
|---|---|
| task queue | `0x18` status, `0x20` result size, `0x28` cancel, `0x30` run, `0x38` begin, `0x40` missing, `0x48` schedule, `0x50` complete, `0x58` get result, `0x60` create, `0x68` create composite, `0x70` get port, `0x78` duplicate handle, `0x80` dispatch, `0x88` close handle, `0x90` submit, `0x98` submit delayed, `0xa0`/`0xa8` missing wait handles, `0xb0` terminate, `0xb8` register monitor, `0xc0` unregister monitor, `0xc8` get process queue, `0xd0` set process queue, `0xd8`/`0xe0` missing, `0xe8` thread check, `0xf0` missing |
| user | `0x18` duplicate, `0x20` close, `0x28` compare, `0x30` missing, `0x38` add async, `0x40` add result, `0x48` local id, `0x50` find by local id, `0x58` id, `0x60` find by id, `0x68` guest, `0x70` state, `0x78` gamertag, `0x80`-`0x90` missing, `0x98` age group, `0xa0` check privilege, `0xa8` resolve privilege, `0xb0` resolve result, `0xb8` token, `0xc0` token size, `0xc8` token result, `0xd0`-`0x100` missing, `0x108` register event, `0x110` unregister event |
| user gamertag | `0x18` gamertag |
| user devices | `0x18` find for device, `0x20` register event, `0x28` unregister event |
| system | `0x18` console id, `0x20` sandbox, `0x28` device id |
| analytics | `0x18` information |
| title | `0x18` title id, `0x20` launch |
| features | `0x18` available |
| invites | `0x18` register event, `0x20` unregister event |
| events | `0x18` write event |

## Async block

| Offset | Size | Field |
|---|---|---|
| `0x00` | 8 | Task-queue handle, null for the process queue |
| `0x08` | 8 | Provider context |
| `0x10` | 8 | Completion routine |
| `0x18` | 8 | Unused |
| `0x20` | 8 | Status, low 32 bits are the `HRESULT` |
| `0x28` | 8 | Result size in bytes |
| `0x30` | 8 | Unused |

Provider operations: 0 begin, 1 do work, 2 get result, 3 cancel, 4 cleanup.

## Task queues

| Item | Value |
|---|---|
| Modes | 0 manual, 1 thread pool, 2 serialized, 3 immediate |
| Ports | 0 work, 1 completion |
| Infinite timeout | `0xFFFFFFFF` |
| Status values | `E_PENDING 0x8000000A`, `E_ABORT 0x80004004`, `E_NOTIMPL 0x80004001`, `E_INVALIDARG 0x80070057`, `E_NOT_SUFFICIENT_BUFFER 0x8007007A`, `E_ILLEGAL_METHOD_CALL 0x8000000E` |

## System information

| Item | Value |
|---|---|
| Console id | `00000000000000000000000000000000000001` (38 characters) |
| Sandbox | `RETAIL` |
| Device id | `VkljdG9yeSBvZmZsaW5lIGRldmljZSBpZGVudGl0eSE=` (44 characters) |
| OS and hosting OS | 10.0.22000.0 |
| Analytics family | `Xbox` |
| Analytics form | `Xbox Series X` |
| `Version` | four `uint16`: major, minor, build, revision (8 bytes) |
| `AnalyticsInfo` | `Version` os at `0x00`, hosting OS at `0x08`, family at `0x10`, form at `0x50`; 0x90 bytes |

## Title id

Read from `MicrosoftGame.config` beside the executable: the text of the element whose path from the root is `Game/TitleId`, parsed as hexadecimal. Comments, CDATA sections, declarations and attributes are skipped. A missing manifest or value reads as 0.
