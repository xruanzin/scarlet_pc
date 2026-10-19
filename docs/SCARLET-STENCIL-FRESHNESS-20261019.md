# Depth/stencil feedback freshness

The 12:40 capture includes the new MRT snapshots. Hair base colour is neutral in
RT1, while RT4 contains encoded directional data. Lighting selects an anisotropic
material path using stencil bits 3–5; treating the same RT4 values as ordinary
emission can produce coloured glowing hair. The capture does not include stencil
bytes, so it cannot prove which material branch actually ran.

Two real snapshot defects were found and corrected:

- Draw invalidation considered only depth writes, ignoring stencil-only writes.
  That rule came from a depth-only consumer and is insufficient now that the
  lighting shader samples stencil material IDs too. Front/back operations and
  write masks now participate in invalidation.
- Feedback reuse allowed an invalid snapshot whenever the current draw wrote
  depth. Writing an attachment does not make an earlier copy current. Reuse now
  requires the cached snapshot to be valid.

F9 additionally saves up to eight source/snapshot stencil pairs as raw 8-bit PGM
files and logs stencil state per draw. They are included by the existing forensic
collector. Snapshot images now include TRANSFER_SRC usage for these readbacks
(and the existing opt-in depth readback). Normal frames do not perform readbacks.

Validation: native tests cover stencil-only writes, KEEP operations, disabled
write masks, back-face writes, normal depth writes and absent attachments. The
modified executor compiles. Existing sampled-view and copy-plane tests pass.
No Windows/AMD game validation was available; these cache fixes are not a claim
that every hair or postprocessing defect is solved. F9 readback timings are not
representative of ordinary gameplay performance.
