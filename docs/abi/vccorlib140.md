# vccorlib140.dll and console WinRT classes

Games activate WinRT classes through `vccorlib140.dll`; g6shim's `g6shim_vccorlib140.dll` serves the console classes. Method numbers count from 0 after `IInspectable` (method 0 is vtable slot 6).

## Activation

| Item | Value |
|---|---|
| Export | `?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z` |
| Signature | `HRESULT (PCWSTR class_name, Platform::Guid& iid, void** factory)`; `Platform::Guid` has the layout of a `GUID` |
| Console namespaces | `Windows.Xbox.*`, `Windows.Kinect.*` |
| Package classes | `AppxManifest.xml` `<InProcessServer>` blocks: `<Path>` and `ActivatableClassId` attributes; activated through the DLL's `DllGetActivationFactory` |
| Console manifest | `AppxManifest.console.xml` when the game is registered as a PC package |

## App model

The console's `CoreApplication`, `CoreApplicationView`, `CoreWindow` and `CoreDispatcher` share IIDs with the PC's but not their method order.

| Class, interface | Method | Meaning |
|---|---|---|
| `CoreApplication` `ICoreApplication` `{0AACF7A4-...}` | 1/2, 3/4 | add/remove `Suspending`, `Resuming` |
| | 6 | `GetCurrentView` |
| | 7 | `Run(IFrameworkViewSource)` |
| `CoreApplication` `ICoreImmersiveApplication` `{1ADA0E3E-...}` | 0, 2 | Main view |
| `CoreApplication` console statics `{9FF3799E-6E83-468C-A5B0-501CE6BB0FA1}` | 0 | 32-bit value |
| | 1/2 | add/remove its change event |
| `CoreApplication` console settings `{42A6A529-3CC6-4867-B5B7-6D1FAF5325F9}` | 1 | `(boolean)` |
| `CoreApplicationView` `{638BB2DB-...}` | 0 | `get_CoreWindow(out)`, or `GetAt(index, out)` when the view is used as a list of views |
| | 1/2 | add/remove `Activated` |
| `CoreWindow` `{79B9D5F2-...}` | 1 | `get_Dispatcher` |
| | 5 | `Activate` |
| | others | Mostly event add/remove pairs |
| `CoreWindow` statics `{4D239005-...}` | 0 | `GetForCurrentThread` |
| `CoreDispatcher` `{60DB2FA8-...}` | 0, 1, 2 | `get_HasThreadAccess`, `ProcessEvents(option)`, `RunAsync(priority, handler, out)` |
| `CoreDispatcher` `ICoreAcceleratorKeys` `{9FFDF7F5-...}` | 0/1 | add/remove `AcceleratorKeyActivated` |

`IFrameworkViewSource` slot 6 is `CreateView`; `IFrameworkView` slots 6-10 are `Initialize`, `SetWindow`, `Load`, `Run`, `Uninitialize`. `Windows.Xbox.ApplicationModel.Core.CoreApplicationContext` statics `{D8D4C59F-B9AC-4D41-842D-A01C59B617E7}` method 0 returns an object; method 1 adds an event handler.

## Input

| Class, interface | Method | Meaning |
|---|---|---|
| `Windows.Xbox.Input.Controller` statics `{7400624F-144B-47FC-9F3B-1DCAFB482873}` | 0 | Controller list (`IVectorView`) |
| | 1/2, 3/4, 5/6 | add/remove controller added, removed, pairing changed |
| `Controller` default interface | 0 | 64-bit ID |
| `IController` `{9776FF56-9BFD-4581-AD45-B645BBA526D6}` | 0, 1 | 64-bit ID, type name (`HSTRING`) |
| `Windows.Xbox.Input.Gamepad` statics `{8C4C1BCD-FDAB-4BF8-BDE9-ABB2B1D6FDD3}` | 0 | Gamepad list |
| | 1/2, 3/4 | add/remove gamepad events |
| `Gamepad` default interface | 0 | `(const float[4])`: both motors, both triggers |
| | 1 | Reading object (below) |
| | 2 | `GetRawCurrentReading(reading*)`, 40-byte reading |
| `Windows.Devices.Input.MouseDevice` statics `{F5A82CE3-4A2D-4004-B386-C74DC69BE979}` | 0 | `GetForCurrentView(out)` |
| `IMouseCapabilities` `{56CB2884-282D-49F7-A1BC-456ECC2E5DE8}` | 0-4 | 32-bit values |

Raw reading, the layout of `Windows.Gaming.Input.GamepadReading`:

| Offset | Content |
|---|---|
| +0 | Timestamp |
| +8 | Buttons: `0x1` Menu, `0x2` View, `0x4` A, `0x8` B, `0x10` X, `0x20` Y, `0x40`-`0x200` D-pad up, down, left, right, `0x400`/`0x800` shoulders, `0x1000`/`0x2000` stick clicks |
| +12, +16 | Left and right trigger, 0 to 1 |
| +20 to +32 | Left X, left Y, right X, right Y, −1 to 1, up positive |

Reading object methods: 0 timestamp, 1 buttons, 2-15 one boolean per button in bit order, 16-21 triggers then stick axes.

## Users

| Class, interface | Method | Meaning |
|---|---|---|
| `Windows.Xbox.System.User` statics `{B4F23A93-7FC6-4A50-B153-950016389E50}` | 0 | 32-bit value |
| | 3 | Users list |
| | 4/5, 6/7, 12, 14 | Events |
| `User` | 0 | 32-bit ID |
| | 2 | List |
| | 3 | Display info object |
| | 4, 5 | Booleans |
| | 7 | Object, may be null |
| | 8, 9 | Strings |
| | 10 | `(method, url, headers, out IAsyncOperation)`: token and signature request |
| | 12 | Same with a request body before the out pointer |
| `UserDisplayInfo` | 0, 2, 3 | Strings |
| | 1, 4, 5 | 32-bit values |
| | 6 | List |
| `Windows.Xbox.UI.SystemUI` statics `{1A0DD80E-3F9D-4CFA-8E50-0E2EBD1AA72F}` | 4 | `(controller, options, out IAsyncOperation)`: account picker |
| `AccountPickerResult` | 0 | User |
| | 2 | Object |

## Storage

| Class, interface | Method | Meaning |
|---|---|---|
| `Windows.Xbox.Storage.ConnectedStorageSpace` statics `{07020414-B3E9-47C8-A36F-F24E2DC65573}` | 0 | `GetForUserAsync(user, out)` |
| `ConnectedStorageSpace` | 3 | `(name, out)`: a container |
| | 5 | `CreateContainer(name, out)` |
| `ConnectedStorageContainer` | 1 | Its storage space |
| | 2 | `SubmitUpdatesAsync(IMapView<String, IBuffer> writes, IIterable<String> deletes, out IAsyncAction)` |
| | 4 | `GetAsync(IIterable<String> names, out IAsyncOperation<IMapView<String, IBuffer>>)` |

## Services

| Class, interface | Method | Meaning |
|---|---|---|
| `Windows.Xbox.Services.XboxLiveConfiguration` statics `{00193376-6A84-4C67-84A5-3B8A8AAEA712}` | 0 | Title ID as a decimal string, from the manifest's hex `TitleId` |
| | 1, 2 | Strings: service configuration ID and sandbox |
| `Windows.Xbox.Achievements.AchievementNotifier` statics `{82CA180B-30E2-4EA0-A62E-B61697A2F334}` | 0 | Notifier object |
| `Windows.Xbox.Chat.ChatSession` factory `{53CB6065-C211-4B21-A3EF-CD544D22ABA1}` | 0 | `(number, flag, out)`: a session |
| `ChatSession` | 0 | Growable list (`IVector`, `Append` is method 7) |
| `Windows.Xbox.Networking.SecureDeviceAssociationTemplate` statics `{46745A18-697A-47E1-BB62-CC71DE0FDD8C}` | 1 | `(out)`: template list |
| `Windows.Xbox.Management.Deployment.DownloadableContentPackageManager` `{971FEAAF-98F8-4038-B8C1-3A5F1A043B97}` | 0 | `(filter, out IVectorView)`: installed packages |
| | 1/2 | add/remove install completed |

The manifest element `XboxLive` carries `TitleId` and `PrimaryServiceConfigId`.

## Depth camera

| Class, interface | Method | Meaning |
|---|---|---|
| `Windows.Kinect.KinectSensor` statics `{C421D46B-5084-45D6-BDF0-57151F81D0D9}` | 0 | `GetDefault(out)` |
| Sensor | 0/1 | add/remove an event |
| | 2, 3 | Open, Close |
| | 4, 5 | Booleans: is open, is available |
| | 6 | Object; its method 4 `(5, out)` returns a frame description |
| Frame description | 0, 1 | Width, height |
