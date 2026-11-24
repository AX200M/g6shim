# shader

## add Functional

- 64-bit image atomic increment and decrement stop the translation: shader model 6.6 has no increment or decrement atomic.
  Closes when: a title uses one.
- A 64-bit image atomic needs shader model 6.6 typed UAV atomics. A host without `D3D12_FEATURE_DATA_D3D12_OPTIONS9::AtomicInt64OnTypedResourceSupported`, or without `D3D12_FEATURE_DATA_D3D12_OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported`, refuses the pipeline.
  Closes when: those hosts are rejected before pipeline creation with a reported reason.
