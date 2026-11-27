# shader

## add Unknown function info

- A geometry-bound pipeline is assumed to select the export-shader payload on the console. The host runs the paired container's legacy vertex payload, and the geometry shader reads that vertex shader's exports instead of the export-shader ring.
  Closes when: the payload a geometry-bound pipeline selects is confirmed.

## add Functional

- A geometry shader's maximum vertex count is 1024 divided by its output scalar count, the DXIL limit, rather than the console's declared maximum. The declared maximum is not read from the stage data.
  Closes when: the stage-data field for the maximum is known.
- Geometry output is stream 0 only. Stream output, streams 1-3, adjacency primitives and geometry instancing are refused.
  Closes when: a title uses them.
