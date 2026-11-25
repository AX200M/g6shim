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
| Analytics form | `Xbox Series X`, or `Xbox Series S` when the console model is Series S |
| `Version` | four `uint16`: major, minor, build, revision (8 bytes) |
| `AnalyticsInfo` | `Version` os at `0x00`, hosting OS at `0x08`, family at `0x10`, form at `0x50`; 0x90 bytes |

## Title id

Read from `MicrosoftGame.config` beside the executable: the text of the element whose path from the root is `Game/TitleId`, parsed as hexadecimal. Comments, CDATA sections, declarations and attributes are skipped. A missing manifest or value reads as 0.

## Title data

| Item | Value |
|---|---|
| Store id | `Game/StoreId` from the same manifest; empty when absent |
| Persistent local storage quota | `Game/PersistentLocalStorage/SizeMB` × 1 MiB; 0 when absent |
| Data folder | `G6SHIM_DATA_DIR`, or `[saves] dir` in `g6shim.ini`, else `%LOCALAPPDATA%\g6shim\data\<title id>`; the title id is 8 uppercase hex digits |
| Game saves | `<data folder>\saves\<xuid, 16 uppercase hex>\<service configuration>\`, the configuration `default` when empty |
| Persistent local storage folder | the folder behind `drives::persistent_storage_drive()`: `g6shim_persistent` next to the executable |

## Areas served (part 2)

| Area | Provider | Interface | Object |
|---|---|---|---|
| persistent local storage | `f4faf4d4-2d04-4fce-b3e0-474a713a3e84` | `d29411df-0794-4553-8b27-95fc02d0f75d`, `41a4e10c-5a7e-41d9-8c37-37bde62a07d6` | `persistent local storage` |
| networking | `37e56907-2f10-41e8-b72f-36edb185331a` | the provider, `bf2346b2-39af-4658-b5ea-44713c7e83b3` | `networking` |
| package | `af406016-e850-4aa8-a88d-2f3dcb9dac7e` | `0b9d556e-ab70-43f4-baa0-9999ac4cb040` | `package` |
| hdr | `03f0fe74-fdd9-4e5c-b630-f9339c47acc5` | `35f07670-706e-4bfb-9476-090798c5ebf3` | `hdr` |
| shell | `1b339674-328d-4283-a200-3171f18d3639` | the provider | `shell` |
| capture | `a4f1aee2-4bf1-4485-b008-a7c26d52ac27` | `22e672d7-b4e3-406c-bd50-8f0d25236f9e` | `capture` |
| game save | `704c3f58-e629-4cc2-b197-30511b996fe2` | the provider, `1bfff3af-f14a-40a3-8e35-9ada906593f9` | `game save` |
| store | `0dd112ac-7c24-448c-b92b-3960fb5bd30c` | `b09d803c-2414-4a05-82c6-66dfdc9e9a44`, `2d42fea5-e71d-4b76-97cd-c50afbb3ae5d` | `store` |
| game ui | `dfcd4649-4ff8-4043-ba07-35d607df98b0` | `eaf669df-5542-4590-99a3-8dc061f837cc` | `game ui` |
| accessibility | `3e241977-6237-41e9-8559-102c2d9983f1` | `d722b373-8c4d-4692-9e51-d6ad9b37aa7d` | `accessibility` |

### Method slots

| Object | Slots |
|---|---|
| persistent local storage | `0x18` path size, `0x20` path, `0x28` space, `0x30` prompt, `0x38` prompt result |
| networking | `0x18` UDP port, `0x20`/`0x28` port async/result, `0x40` security info, `0x48` size, `0x50` result, `0x70` verify certificate, `0x78` connectivity hint, `0x80`/`0x88` hint registration |
| package | `0x18` identifier, `0x20` is packaged, `0x28`/`0x30` create/close monitor, `0x38` progress, `0x48` register progress, `0x60` find availability, `0x70` change order, `0x80`/`0x88` install chunks async/result, `0x90` estimate download, `0x98` uninstall chunks, `0xc8` mount path, `0xd0` close mount, `0x110` uninstall package, `0x128`/`0x130` mount async/result, `0x138` enumerate |
| hdr | `0x20` enable HDR |
| shell | `0x18` launch URI, `0x20` acquire deferral, `0x28` close deferral |
| capture | `0x28` screenshot |
| game save | `0x20`/`0x28` initialize async/result, `0x30` close provider, `0x40`/`0x48` quota async/result, `0x58`/`0x60` delete async/result, `0x68` container info, `0x70` enumerate containers, `0x80`/`0x88` create/close container, `0x98` enumerate blobs, `0xa0` read blobs, `0xa8`/`0xb0` read async/result, `0xb8`/`0xc0` create/close update, `0xc8` write blob, `0xd0` delete blob, `0xe0`/`0xe8` submit async/result |
| store | `0x18`/`0x20` create/close context, `0x28` associated products, `0x30` query result, `0x48` entitled products, `0x50`/`0x90` query result, `0x78` enumerate products, `0x80` has more pages, `0x88` next page, `0x98` close query, `0xa0`/`0xa8` licence for package async/result, `0xb0` licence valid, `0xb8` close licence, `0xe0`/`0xe8` game licence async/result, `0x108` consumable balance, `0x110`/`0x1e8` status, `0x118` report fulfilment, `0x128`/`0x140` collections/purchase id, `0x130`/`0x148` size, `0x138`/`0x150` string, `0x188` purchase page, `0x1a8` redeem page, `0x1b8` updates, `0x1c0`/`0x1f8` count, `0x1c8`/`0x200` array, `0x1e0`/`0x1f0` install, `0x208` package identifier, `0x210`-`0x228` licence change registration, `0x238`/`0x240` durable licence async/result, `0x258` product page |
| game ui | `0x38`/`0x40` profile card async/result, `0x88`/`0x90`/`0x98` text entry async/size/result |
| accessibility | `0x40` enumerate voices, `0x48` create, `0x50` close, `0x58` default voice, `0x60` custom voice, `0x68` stream from text, `0x70` close stream, `0x78` stream size, `0x80` stream data, `0xa8` stream from SSML |

## Structures

| Item | Layout |
|---|---|
| `SpaceInfo` | `available_free` 0x00, `total_free` 0x08, `used` 0x10, `total` 0x18; 0x20 bytes |
| `ConnectivityHint` | `level` 0x00, `cost` 0x04, `interface_type` 0x08, `initialized`/`approaching_data_limit`/`over_data_limit`/`roaming` at 0x0c-0x0f; 0x10 bytes |
| `Progress` | `total_bytes` 0x00, `installed_bytes` 0x08, `launch_bytes` 0x10, `launchable` 0x18, `completed` 0x19; 0x20 bytes |
| `ContainerInfo` | `name` 0x00, `display_name` 0x08, `blob_count` 0x10, `total_size` 0x18, `last_modified` 0x20, `needs_sync` 0x28; 0x30 bytes |
| `BlobInfo` | `name` 0x00, `size` 0x08; 0x10 bytes |
| blob entry | `name` pointer 0x00, size 0x08 (u32), data pointer 0x10; 0x18 bytes |
| `GameLicense` | `sku_store_id` 0x00 (18 bytes), `is_active` 0x12, `is_trial_owned_by_this_user` 0x13, `is_disc_license` 0x14, `is_trial` 0x15, `trial_seconds_remaining` 0x18, `trial_unique_id` 0x1c (64 bytes), `expiration` 0x60; 0x68 bytes |

## Values

| Item | Value |
|---|---|
| Persistent storage path | `X:\` — the drive root, never a host folder; a buffer of 3 bytes or less fails with `E_FAIL 0x80004005` |
| UDP multiplayer port | 3074 |
| Offline connectivity hint | level 1, cost 0, interface 0, initialized |
| Networking unreachable | `0x800704CF` |
| Package identifier | the title id as 8 uppercase hex digits |
| Package progress | total = installed = launch = the executable folder's bytes, launchable and completed |
| Store network error | `0x89245301` |
| Game licence SKU | `<store id>/0010`, active, not a trial, no expiry |
| Accessibility silence | a valid 16 kHz mono 16-bit PCM WAVE of 44 bytes with no samples |

## Game save files

A provider root is `<saves>\<xuid, 16 uppercase hex>\<configuration>`. A container is a folder `<name with unsafe bytes %XX, cut to 100>~<FNV-1a 64 of the exact name>`. Committed versions are `v<N>` folders; `<N>.new` is one being written and `.gone` one being deleted. A version holds `manifest` and the blobs as files `0`, `1`, ....

The manifest is a sequence of `u32` little-endian lengths and their UTF-8 bytes: container name, display name, then each blob name. Quota 256 MiB per user and title, one update at most 16 MiB, a manifest over 1 MiB or a blob over 64 MiB treated as damaged.
