# winrt

## Build

None.

## Silent stubs

- Gamepad method 0 (vibration) accepts four motor values and does nothing.
  Title IDs: 3DD7389B
- The depth-camera sensor (`Windows.Kinect.KinectSensor`) accepts Open, Close and event removal without a device.
  Title IDs: 3DD7389B
- `CoreApplication` console settings (`{42A6A529-...}` method 1) accept a boolean and ignore it.
  Title IDs: 3DD7389B
- Every probe answers `IClosable.Close` with `S_OK`.
- Async objects complete at once; `Cancel` and `Close` do nothing.
- `CoreDispatcher.RunAsync` reports completion before its callback runs; the callback runs on the next `ProcessEvents`.

## Unknown function info

- Probes stand in for unknown classes and interfaces: a method no stand-in implements logs its slot and returns `E_NOTIMPL`, unless its arguments look like an event add or remove.
- `User` methods are guessed from use: method 3 returns display info, methods 4 and 5 are booleans (5 taken as signed in), method 7 is an object that may be null, method 8 a name, method 9 a string taken as the user ID.
  Title IDs: 34BA1DE0, 1738CA2A, 3DD7389B
- `UserDisplayInfo` methods 1, 4 and 5 return 32-bit values of unknown meaning (0); method 6 and `User` method 2 return lists of unknown content (empty).
  Title IDs: 1738CA2A
- `User` statics method 0 returns a 32-bit value the title compares with 1 and 2; it returns 0.
  Title IDs: 3DD7389B
- `AccountPickerResult` method 2 returns an unidentified object (a probe).
  Title IDs: 34BA1DE0
- `CoreApplicationContext` statics method 0 is taken as the current user.
  Title IDs: 1738CA2A, 3DD7389B
- `XboxLiveConfiguration` methods 1 and 2 may be swapped (service configuration ID and sandbox).
  Title IDs: 34BA1DE0
- `CoreApplication` console statics `{9FF3799E-...}` method 0 returns a 32-bit value taken as a state (0).
  Title IDs: 34BA1DE0
- The sensor's object at method 6 and that object's method 4 are taken as a frame description (width 512, height 424).
  Title IDs: 3DD7389B
- `ChatSession` factory method 0 takes a number and a flag of unknown meaning.
  Title IDs: 3DD7389B
- `SecureDeviceAssociationTemplate` statics method 1 is taken as the template list.
  Title IDs: 3DD7389B

## Functional

- The depth-camera sensor reports no device: IsOpen and IsAvailable are false and no frame arrives.
  Title IDs: 3DD7389B
- Token requests fail: `GetTokenAndSignatureAsync` completes with `ERROR_NETWORK_UNREACHABLE`, so titles run offline.
  Title IDs: 3DD7389B
- One local user (ID 1, name "Player"); no sign-in, profiles or controller pairing.
- No downloadable content is reported and install events never fire.
  Title IDs: 34BA1DE0
- No mouse is reported and `MouseMoved` never fires.
  Title IDs: 1738CA2A
- Window events other than activation never fire; keys come from g6shim's input mapping. `Suspending` and `Resuming` never fire.
- Package activation and the title configuration read `AppxManifest.xml` only.
